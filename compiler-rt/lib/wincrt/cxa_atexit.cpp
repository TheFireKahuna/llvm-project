//===-- cxa_atexit.cpp - Static and quick-exit registries -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One registry for __cxa_atexit and one for __cxa_at_quick_exit. Every
// registering image has a record with an arena of entries and a lock-free
// stack over them, so an unloading image drains and releases exactly its own
// entries. Every entry carries a global sequence number, and the process-wide
// drain merges the records' stacks by it, which is the order one list would
// give: reverse registration across every image, with an entry registered
// during the drain running next.
//
// Nothing here takes a lock. Registration is a sequence increment and a CAS
// push, and consumption a CAS pop shared by the exit drain and an image's
// detach, so each entry runs once. Stack heads are {tag, slot} words, so a
// reused slot cannot confuse a concurrent pop. A record's arenas are freed
// only while no drainer is active, and memory never grows with the number of
// images loaded over time, since records and slots are reused.
//
// The state is the process's, which every copy of the registries shares
// (exit_state.h): records come from the process heap, so any copy can release
// any of them. The Universal CRT orders the registries among other runtimes:
// the executable's start-up posts one _crt_atexit token, and exit runs the
// token, in reverse order with the host's own atexit functions, to drain
// them. Under an executable that reserves no state, each copy keeps its own:
// one that lives as long as the process (clang_rt.wincrt_dynamic.dll, which
// pins itself, or the copy that -static links into an executable) posts a
// token when it is first used, and one that -static links into a DLL posts
// nothing, since the token cannot be withdrawn before the DLL is unloaded,
// and drains at the DLL's detach instead.
//
//===----------------------------------------------------------------------===//

#include "exit_state.h"

#include <process.h>

using namespace wincrt;

// Weak, so that only an image that registers a thread-local destructor links
// their registry.
extern "C" void __cxa_thread_finalize(void *) __attribute__((weak));

namespace {

constexpr uint32_t NoSlot = UINT32_MAX;

using Entry = ExitEntry;
using Image = ExitImage;
using Registry = ExitRegistry;

// A stack head: the top slot, and a tag that every update advances.
uint64_t makeHead(uint64_t Tag, uint32_t Slot) { return Tag << 32 | Slot; }
uint32_t headSlot(uint64_t Head) { return static_cast<uint32_t>(Head); }
uint64_t headTag(uint64_t Head) { return Head >> 32; }

// The keys of registrations without a DSO handle, which the Itanium ABI
// allows, and of a record that its image's detach retired. The process's
// state may outlive this copy, so the keys are the executable's base, which
// no image registers with, and that plus one.
void *noDso() { return NtCurrentTeb()->ProcessEnvironmentBlock->Reserved3[1]; }
void *retiredDso() { return static_cast<char *>(noDso()) + 1; }

enum : LONG { NoToken, PostingToken, TokenPosted };

template <typename T> T load(T *P) {
  return __atomic_load_n(P, __ATOMIC_ACQUIRE);
}
template <typename T> void store(T *P, T Value) {
  __atomic_store_n(P, Value, __ATOMIC_RELEASE);
}
template <typename T> bool cas(T *P, T &Expected, T Desired) {
  return __atomic_compare_exchange_n(P, &Expected, Desired, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Slots
//===----------------------------------------------------------------------===//

unsigned chunkOf(uint32_t Slot, uint32_t &Offset) {
  unsigned K = 31 - __builtin_clz(Slot / ExitFirstChunkEntries + 1);
  Offset = Slot - ExitFirstChunkEntries * ((1u << K) - 1);
  return K;
}

Entry *slotEntry(Image *I, uint32_t Slot) {
  uint32_t Offset;
  unsigned K = chunkOf(Slot, Offset);
  return &load(&I->Chunks[K])[Offset];
}

Entry *allocateSlot(Image *I) {
  for (uint64_t Head = load(&I->Free);;) {
    uint32_t Slot = headSlot(Head);
    if (Slot == NoSlot)
      break;
    Entry *E = slotEntry(I, Slot);
    if (cas(&I->Free, Head, makeHead(headTag(Head) + 1, load(&E->Next))))
      return E;
  }
  uint32_t Slot = __atomic_fetch_add(&I->Bump, 1u, __ATOMIC_ACQ_REL);
  uint32_t Offset;
  unsigned K = chunkOf(Slot, Offset);
  if (K >= ExitMaxChunks)
    return nullptr;
  Entry *Chunk = load(&I->Chunks[K]);
  if (!Chunk) {
    auto *Fresh = static_cast<Entry *>(
        crtAlloc(sizeof(Entry) * (ExitFirstChunkEntries << K)));
    if (!Fresh)
      return nullptr;
    Entry *Expected = nullptr;
    if (cas(&I->Chunks[K], Expected, Fresh)) {
      Chunk = Fresh;
    } else {
      crtFree(Fresh);
      Chunk = Expected;
    }
  }
  Entry *E = &Chunk[Offset];
  E->Slot = Slot;
  return E;
}

void pushHead(uint64_t *Head, Entry *E) {
  for (uint64_t Old = load(Head);;) {
    store(&E->Next, headSlot(Old));
    if (cas(Head, Old, makeHead(headTag(Old) + 1, E->Slot)))
      return;
  }
}

// Pops the newest entry, or returns null. A head made stale by a concurrent
// pop or a reused slot fails the CAS through its tag.
Entry *popHead(Image *I, uint64_t *Head) {
  for (uint64_t Old = load(Head);;) {
    uint32_t Slot = headSlot(Old);
    if (Slot == NoSlot)
      return nullptr;
    Entry *E = slotEntry(I, Slot);
    if (cas(Head, Old, makeHead(headTag(Old) + 1, load(&E->Next))))
      return E;
  }
}

//===----------------------------------------------------------------------===//
// Records
//===----------------------------------------------------------------------===//

Image *findImage(Registry &R, void *Dso) {
  Image *Hint = load(&R.Hint);
  if (Hint && load(&Hint->Dso) == Dso)
    return Hint;
  for (Image *I = load(&R.Images); I; I = I->Next) {
    if (load(&I->Dso) == Dso) {
      store(&R.Hint, I);
      return I;
    }
  }
  return nullptr;
}

// Two threads may create two records for one image; every drain visits all
// of them.
Image *claimImage(Registry &R, void *Dso) {
  if (Image *I = findImage(R, Dso))
    return I;
  for (Image *I = load(&R.Images); I; I = I->Next) {
    void *Expected = nullptr;
    if (cas(&I->Dso, Expected, Dso))
      return I;
  }
  auto *I = static_cast<Image *>(crtAlloc(sizeof(Image)));
  if (!I)
    return nullptr;
  *I = Image();
  I->Dso = Dso;
  I->Live = makeHead(0, NoSlot);
  I->Free = makeHead(0, NoSlot);
  for (Image *Head = load(&R.Images);;) {
    I->Next = Head;
    if (cas(&R.Images, Head, I))
      return I;
  }
}

void pushRetired(Registry &R, Image *I) {
  for (Image *Head = load(&R.Retired);;) {
    I->NextRetired = Head;
    if (cas(&R.Retired, Head, I))
      return;
  }
}

void retire(ExitState &S, Registry &R, void *Dso) {
  for (Image *I = load(&R.Images); I; I = I->Next) {
    void *Expected = Dso;
    if (!cas(&I->Dso, Expected, retiredDso()))
      continue;
    I->RetiredIn = __atomic_load_n(&S.Epoch, __ATOMIC_SEQ_CST);
    pushRetired(R, I);
  }
}

// Frees the arenas of every record retired two epochs before Current, and
// makes it free; a later one waits. A record is free only once it has been
// reset, so a registration that claims it starts from an empty record.
void release(Registry &R, uint32_t Current) {
  for (Image *I = __atomic_exchange_n(&R.Retired, static_cast<Image *>(nullptr),
                                      __ATOMIC_ACQ_REL);
       I;) {
    Image *Next = I->NextRetired;
    if (static_cast<int32_t>(Current - I->RetiredIn) < 2) {
      pushRetired(R, I);
      I = Next;
      continue;
    }
    for (Entry *&Chunk : I->Chunks) {
      if (Chunk)
        crtFree(Chunk);
      Chunk = nullptr;
    }
    I->Live = makeHead(headTag(I->Live) + 1, NoSlot);
    I->Free = makeHead(headTag(I->Free) + 1, NoSlot);
    I->Bump = 0;
    store(&I->Size, uintptr_t(0));
    store(&I->Dso, static_cast<void *>(nullptr));
    I = Next;
  }
}

// Returns the epoch the caller drains in, to pass to leaveDrainer. A drainer
// counted in an epoch that has already ended counts again in the new one.
uint32_t enterDrainer(ExitState &S) {
  for (;;) {
    uint32_t E = __atomic_load_n(&S.Epoch, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&S.Drainers[E & 1], 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&S.Epoch, __ATOMIC_SEQ_CST) == E)
      return E;
    __atomic_sub_fetch(&S.Drainers[E & 1], 1, __ATOMIC_SEQ_CST);
  }
}

void leaveDrainer(ExitState &S, uint32_t Entered) {
  __atomic_sub_fetch(&S.Drainers[Entered & 1], 1, __ATOMIC_SEQ_CST);
  if (!load(&S.Normal.Retired) && !load(&S.Quick.Retired))
    return;
  uint32_t E = __atomic_load_n(&S.Epoch, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&S.Drainers[(E + 1) & 1], __ATOMIC_SEQ_CST) == 0 &&
      __atomic_compare_exchange_n(&S.Epoch, &E, E + 1, false, __ATOMIC_SEQ_CST,
                                  __ATOMIC_SEQ_CST))
    ++E;
  release(S.Normal, E);
  release(S.Quick, E);
}

//===----------------------------------------------------------------------===//
// Draining
//===----------------------------------------------------------------------===//

// A counted reference to the image that contains an address, kept while
// consecutive callbacks stay in that image, so that another thread's
// FreeLibrary cannot unmap code or data a callback is using.
class ModuleReference {
public:
  ~ModuleReference() { reset(); }

  // Returns false if Address is in an image that refuses a reference, which
  // is one in its detach: an address outside every image needs none.
  bool hold(const void *Address) {
    auto Value = reinterpret_cast<uintptr_t>(Address);
    if (Value - Begin < Size)
      return true;
    reset();
    if (!Address)
      return true;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            static_cast<LPCWSTR>(Address), &Module)) {
      Module = nullptr;
      void *Base;
      return !RtlPcToFileHeader(const_cast<void *>(Address), &Base);
    }
    Begin = reinterpret_cast<uintptr_t>(Module);
    Size = imageSize(Module);
    return true;
  }

private:
  void reset() {
    if (Module)
      FreeLibrary(Module);
    Module = nullptr;
    Begin = Size = 0;
  }

  HMODULE Module = nullptr;
  uintptr_t Begin = 0;
  uintptr_t Size = 0;
};

// Runs, or with Execute false discards, the pending entries of the records
// of Dso, or of every live record if Dso is null, newest first. The caller is
// a drainer. Unless the caller holds the loader lock, which keeps every image
// mapped, an entry runs only with references to the images of its callback
// and its DSO, taken before it is claimed so that no FreeLibrary can unmap
// either in between; a record whose image refuses a reference is unloading,
// and its own detach drain runs its entries.
void drain(ExitState &S, Registry &R, void *Dso, bool Execute, bool Reference) {
  ModuleReference Code, Data;
  // The records skipped this round; once there are more than it holds, the
  // round ends and the drainer yields to their detaches.
  constexpr unsigned MaxUnloading = 8;
  void *Unloading[MaxUnloading];
  unsigned NumUnloading = 0;
  auto skip = [&](void *Key) {
    if (NumUnloading == MaxUnloading) {
      NumUnloading = 0;
      YieldProcessor();
      return;
    }
    Unloading[NumUnloading++] = Key;
  };
  auto skipped = [&](void *Key) {
    for (unsigned I = 0; I != NumUnloading; ++I)
      if (Unloading[I] == Key)
        return true;
    return false;
  };
  for (;;) {
    Image *Best = nullptr;
    uint32_t BestSlot = NoSlot;
    uint64_t BestSeq = 0;
    for (Image *I = load(&R.Images); I; I = I->Next) {
      void *Key = load(&I->Dso);
      if (Dso ? Key != Dso : !Key || Key == retiredDso() || skipped(Key))
        continue;
      uint32_t Slot = headSlot(load(&I->Live));
      if (Slot == NoSlot)
        continue;
      uint64_t Seq = load(&slotEntry(I, Slot)->Seq);
      if (!Best || Seq > BestSeq) {
        Best = I;
        BestSlot = Slot;
        BestSeq = Seq;
      }
    }
    if (!Best)
      return;
    void *Key = load(&Best->Dso);
    if (Execute && Reference) {
      // The drainer's epoch keeps the record's arena readable, whoever pops
      // the entry first.
      Entry *Top = slotEntry(Best, BestSlot);
      void *Callback =
          &R == &S.Quick
              ? load(&Top->Obj)
              : reinterpret_cast<void *>(decodeExit(S, load(&Top->Dtor)));
      if (!Code.hold(Callback) || !Data.hold(Key == noDso() ? nullptr : Key)) {
        skip(Key);
        continue;
      }
    }
    // A concurrent drainer may pop the entry first; the record then offers
    // its next one on the following round.
    Entry *E = popHead(Best, &Best->Live);
    if (!E)
      continue;
    Destructor Dtor = decodeExit(S, E->Dtor);
    void *Obj = E->Obj;
    bool Salted = E->Seq & 1;
    // The entry popped differs from the one referenced if another drainer
    // popped that one in between. If its callback's image is unloading, the
    // entry goes back to its record for that image's detach drain.
    if (Execute && Reference &&
        !Code.hold(&R == &S.Quick ? Obj : reinterpret_cast<void *>(Dtor))) {
      pushHead(&Best->Live, E);
      skip(Key);
      continue;
    }
    pushHead(&Best->Free, E);
    if (!Execute)
      continue;
    invokeCallback(Dtor, Obj, Salted);
  }
}

void drainAll(ExitState &S, Registry &R) {
  uint32_t Entered = enterDrainer(S);
  drain(S, R, nullptr, true, true);
  leaveDrainer(S, Entered);
}

//===----------------------------------------------------------------------===//
// Universal CRT tokens
//===----------------------------------------------------------------------===//

// Runs before every function in the Universal CRT's atexit table, including
// those another runtime registered after this registry's token, so that the
// exiting thread's thread-local objects are destroyed before any static one.
void NTAPI threadExitCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_DETACH)
    __cxa_thread_finalize(nullptr);
}

void __cdecl normalToken() {
  ExitState &S = exitState();
  // Nothing is left here after threadExitCallback, unless the executable
  // could not register it.
  if (__cxa_thread_finalize)
    __cxa_thread_finalize(nullptr);
  drainAll(S, S.Normal);
  store(&S.Drained, 1L);
  if (uintptr_t Hook = load(&S.AfterDrain))
    reinterpret_cast<void (*)()>(decodeExit(S, Hook))();
}

void __cdecl quickToken() {
  ExitState &S = exitState();
  drainAll(S, S.Quick);
}

// Returns whether the registry has its token, or needs none. Under the
// process's state only the executable's start-up posts it; before then a
// registration needs none, since the start-up posts it before any of its
// constructors runs. A registrant that finds another posting it waits for the
// outcome, and posts the token itself if that failed, so that no entry is
// left without one. The poster is inside _crt_atexit, which holds the
// Universal CRT's lock, so a poster that never returns has stopped the
// process's C runtime anyway.
bool postToken(ExitState &S, Registry &R, bool Executable = false) {
  if (load(&R.Token) == TokenPosted)
    return true;
  if (isProcessExitState() && !Executable)
    return true;
#ifndef COMPILER_RT_SHARED_LIB
  if (!load(&S.ExecutableRegistered))
    return true;
#endif
  for (LONG Expected = NoToken;
       !cas(&R.Token, Expected, static_cast<LONG>(PostingToken));
       Expected = NoToken) {
    if (Expected == TokenPosted)
      return true;
    YieldProcessor();
  }
  if (&R == &S.Quick ? _crt_at_quick_exit(quickToken)
                     : _crt_atexit(normalToken)) {
    store(&R.Token, static_cast<LONG>(NoToken));
    return false;
  }
  store(&R.Token, static_cast<LONG>(TokenPosted));
  return true;
}

// A destructor registered for an object outside the registering image is one
// of a shared vague entity, an instance that another image owns and others
// use, which the registering image's code initialized: its detach would
// destroy the instance while others use it, so the image is kept loaded, as
// it is only then that it must be. Every other static object is inside the
// image that registers its destructor. Returns false if the image cannot be
// kept loaded.
bool keepRegistrar(Image *I, void *Dso, void *Object) {
  uintptr_t Size = __atomic_load_n(&I->Size, __ATOMIC_ACQUIRE);
  uintptr_t Begin;
  if (Size) {
    Begin = load(&I->Begin);
  } else {
    HMODULE Module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(Dso), &Module))
      return true;
    Begin = reinterpret_cast<uintptr_t>(Module);
    Size = imageSize(Module);
    store(&I->Begin, Begin);
    __atomic_store_n(&I->Size, Size, __ATOMIC_RELEASE);
  }
  if (reinterpret_cast<uintptr_t>(Object) - Begin < Size ||
      reinterpret_cast<HMODULE>(Begin) == GetModuleHandleW(nullptr))
    return true;
  HMODULE Pinned;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            static_cast<LPCWSTR>(Dso), &Pinned);
}

// Object is a static object for the C++ registrations, and an adaptor's
// function for the C ones.
int append(Registry ExitState::*Which, Destructor Function, void *Object,
           void *Dso, bool Salted, bool Static) {
  ExitState &S = exitState();
  Registry &R = S.*Which;
  if (!Function || !postToken(S, R))
    return -1;
  Image *I = claimImage(R, Dso ? Dso : noDso());
  if (!I)
    return -1;
  if (Static && Object && Dso && !keepRegistrar(I, Dso, Object))
    return -1;
  Entry *E = allocateSlot(I);
  if (!E)
    return -1;
  E->Dtor = encodeExit(S, Function);
  E->Obj = Object;
  __atomic_store_n(
      &E->Seq, __atomic_add_fetch(&S.Sequence, 2, __ATOMIC_RELAXED) | Salted,
      __ATOMIC_RELAXED);
  pushHead(&I->Live, E);
  return 0;
}

// The executable's state, found through the locator its link writes, after
// checking that its layout is this copy's; or this copy's own.
ExitState LocalState = {ExitStateVersion, sizeof(ExitState)};
ExitState *State;

ExitState *findExitState() {
  auto *Base = static_cast<char *>(noDso());
  uint32_t Tag, Rva;
  memcpy(&Tag, Base + ExitStateLocator, 4);
  memcpy(&Rva, Base + ExitStateLocator + 4, 4);
  if (Tag != ExitStateTag || !Rva)
    return &LocalState;
  auto *S = reinterpret_cast<ExitState *>(Base + Rva);
  // Two copies that disagree on the layout would corrupt each other's lists.
  if (S->Version != ExitStateVersion || S->Size != sizeof(ExitState))
    __fastfail(FAST_FAIL_INVALID_IMAGE_BASE);
  return S;
}

} // namespace

namespace wincrt {

ExitState &exitState() {
  ExitState *S = load(&State);
  if (!S) {
    S = findExitState();
    store(&State, S);
  }
  return *S;
}

bool isProcessExitState() { return &exitState() != &LocalState; }

ThreadExitState *threadExitState(bool Create) {
  ExitState &S = exitState();
  LONG Slot = load(&S.ThreadSlot);
  if (!Slot) {
    if (!Create)
      return nullptr;
    DWORD Index = TlsAlloc();
    if (Index == TLS_OUT_OF_INDEXES)
      return nullptr;
    LONG Expected = 0;
    if (cas(&S.ThreadSlot, Expected, static_cast<LONG>(Index + 1))) {
      Slot = Index + 1;
    } else {
      TlsFree(Index);
      Slot = Expected;
    }
  }
  // Unlike TlsGetValue, TlsGetValue2 leaves the thread's last error alone.
  auto *T = static_cast<ThreadExitState *>(TlsGetValue2(Slot - 1));
  if (T || !Create)
    return T;
  T = static_cast<ThreadExitState *>(crtAlloc(sizeof(ThreadExitState)));
  if (!T)
    return nullptr;
  *T = ThreadExitState();
  if (!TlsSetValue(Slot - 1, T)) {
    crtFree(T);
    return nullptr;
  }
  return T;
}

void releaseThreadExitState(ThreadExitState *T) {
  TlsSetValue(load(&exitState().ThreadSlot) - 1, nullptr);
  crtFree(T);
}

} // namespace wincrt

extern "C" {

int __cdecl __cxa_atexit(void (*Function)(void *), void *Object, void *Dso) {
  return append(&ExitState::Normal, Function, Object, Dso, false, true);
}

// Clang registers its destructors here, since they carry the salted type.
int __cdecl __llvm_kcfi_cxa_atexit(void (*Function)(void *), void *Object,
                                   void *Dso) {
  return append(&ExitState::Normal, Function, Object, Dso, true, true);
}

// atexit and _onexit register a C function through an adaptor, which carries
// the salted destructor type.
int __cdecl __wincrt_atexit(void (*Adaptor)(void *), void *Function,
                            void *Dso) {
  return append(&ExitState::Normal, Adaptor, Function, Dso, true, false);
}

void __cdecl __cxa_finalize(void *Dso) {
  ExitState &S = exitState();
  if (!Dso) {
    drainAll(S, S.Normal);
    store(&S.Drained, 1L);
    return;
  }
  uint32_t Entered = enterDrainer(S);
  drain(S, S.Normal, Dso, true, true);
  leaveDrainer(S, Entered);
}

int __cxa_at_quick_exit(void (*Function)(void), void *Dso) {
  if (!Function)
    return -1;
  return append(&ExitState::Quick, reinterpret_cast<Destructor>(callVoid),
                reinterpret_cast<void *>(Function), Dso, true, false);
}

// Called by the executable's start-up only, and before any constructor,
// which may call exit. The Universal CRT accepts one thread-exit callback,
// from the executable.
void __wincrt_register_executable(void (*Hook)(void)) {
  ExitState &S = exitState();
  store(&S.AfterDrain, encodeExit(S, reinterpret_cast<Destructor>(Hook)));
  store(&S.ExecutableRegistered, 1L);
  if (__cxa_thread_finalize)
    _register_thread_local_exe_atexit_callback(threadExitCallback);
  postToken(S, S.Normal, /*Executable=*/true);
  if (isProcessExitState())
    postToken(S, S.Quick, /*Executable=*/true);
}

// FreeLibrary always runs the image's registrations. At process exit under a
// wincrt executable, exit's token has already run them, or the process ended
// without running any (_Exit, quick_exit, ExitProcess) and none may run now.
// Under any other host the detach is the image's only termination, as for a
// vcruntime DLL. An unloading image's quick-exit registrations never run.
int __wincrt_detach_image(void *Dso, int Terminating) {
  ExitState &S = exitState();
  if (Terminating && load(&S.ExecutableRegistered) && !load(&S.Drained))
    return 0;
  if (__cxa_thread_finalize)
    __cxa_thread_finalize(Dso);
  // The loader lock is held, so no reference is needed.
  uint32_t Entered = enterDrainer(S);
  drain(S, S.Normal, Dso, true, false);
  drain(S, S.Quick, Dso, false, false);
  retire(S, S.Normal, Dso);
  retire(S, S.Quick, Dso);
  leaveDrainer(S, Entered);
  return 1;
}

#ifdef COMPILER_RT_SHARED_LIB
// The registries serve the process until it ends: the tokens and the
// thread-exit callback they post must stay mapped until exit runs them, and
// the records outlive the images that registered them.
BOOL WINAPI DllMain(HINSTANCE, DWORD Reason, LPVOID) {
  HMODULE Self;
  if (Reason == DLL_PROCESS_ATTACH)
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&DllMain), &Self);
  return TRUE;
}
#endif

} // extern "C"
