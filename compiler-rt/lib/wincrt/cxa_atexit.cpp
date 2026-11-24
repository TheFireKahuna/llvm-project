//===-- cxa_atexit.cpp - The termination registries -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One registry for __cxa_atexit and one for __cxa_at_quick_exit. Every
// registering image has a record, an array of entries that only grows, so an
// unloading image runs and releases exactly its own entries. Every entry
// carries a global sequence number, and the process-wide drain merges the
// records by it, which is the order one list would give: reverse registration
// across every image, with an entry registered during the drain running next.
//
// Nothing here takes a lock or waits. Registration fills the next slot of its
// record and publishes it by storing its sequence number; a drainer claims an
// entry by replacing that number, so each entry runs once. A record's entries
// are freed two epochs after its image's detach retired it, when no drainer
// can still read them, and the record is then reused.
//
// The state is the process's, which every copy of the registries shares
// (wincrt.h): records come from the process heap, so any copy can release any
// of them. The Universal CRT orders the registries among other runtimes: the
// executable's start-up posts one _crt_atexit token, and exit runs the token,
// in reverse order with the host's own atexit functions, to drain them. Under
// an executable that reserves no state, each copy keeps its own:
// clang_rt.wincrt_dynamic.dll, which pins itself, posts a token when it is
// first used, and a copy that -static links into a DLL posts none, since a
// token cannot be withdrawn before the DLL is unloaded, and drains at the
// DLL's detach instead.
//
//===----------------------------------------------------------------------===//

#include "tlsrt.h"

#include <process.h>

using namespace wincrt;

// Weak, so that only an image that registers a thread-local destructor links
// their registry.
extern "C" void __cxa_thread_finalize(void *) __attribute__((weak));

// Chunk K of a record holds FirstChunkEntries << K entries, so MaxChunks
// chunks address every 32-bit slot.
static constexpr uint32_t FirstChunkEntries = 64;
static constexpr unsigned MaxChunks = 26;

// The registrations of one image.
struct wincrt::ExitImage {
  struct Entry {
    uintptr_t Dtor; // encoded with the state's cookie
    void *Obj;
    // Zero until the registration publishes the entry by storing twice its
    // global sequence number, plus one if Dtor was registered as carrying the
    // salted destructor type. A drainer claims the entry by replacing that
    // with Ran, which no sequence number gives.
    uint64_t Seq;
    static constexpr uint64_t Ran = 1;
  };

  // The registering image's handle, retiredDso() from its detach until the
  // record is released, and null while the record is free.
  void *Dso;
  uint32_t Bump; // the next slot never taken
  // No slot from here up holds a pending entry, but for those whose
  // registration has yet to raise it. It is lowered only past a slot whose
  // entry has run, a state that no registration can change, so no entry is
  // ever left above it.
  uint32_t Top;
  Entry *Chunks[MaxChunks];
  ExitImage *Next;
  ExitImage *NextRetired;
  uint32_t RetiredIn; // the epoch of its retirement
  // Set by an exit drain that the image refused a reference: it is unloading,
  // and its detach runs its entries.
  LONG Unloading;
  // The registering image's base and size, once a registration has looked
  // them up; Size is zero until then.
  uintptr_t Begin;
  uintptr_t Size;
};

namespace {

using Image = ExitImage;
using Entry = ExitImage::Entry;
using Registry = ExitRegistry;

// The keys of registrations without a DSO handle, which the Itanium ABI
// allows, and of a record that its image's detach retired. The process's
// state may outlive this copy, so the keys are the executable's base, which
// no image registers with, and that plus one.
void *noDso() { return NtCurrentTeb()->ProcessEnvironmentBlock->Reserved3[1]; }
void *retiredDso() { return static_cast<char *>(noDso()) + 1; }

template <typename T> T load(T *P) {
  return __atomic_load_n(P, __ATOMIC_ACQUIRE);
}
template <typename T, typename U> void store(T *P, U Value) {
  __atomic_store_n(P, static_cast<T>(Value), __ATOMIC_RELEASE);
}
template <typename T, typename U> bool cas(T *P, T &Expected, U Desired) {
  return __atomic_compare_exchange_n(P, &Expected, static_cast<T>(Desired),
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Records
//===----------------------------------------------------------------------===//

// The entry of a slot, whose chunk is made if Allocate is set; null if there
// is no such chunk.
Entry *slotEntry(Image *I, uint32_t Slot, bool Allocate = false) {
  unsigned K = 31 - __builtin_clz(Slot / FirstChunkEntries + 1);
  if (K >= MaxChunks)
    return nullptr;
  Entry *Chunk = load(&I->Chunks[K]);
  if (!Chunk && Allocate) {
    auto *Fresh = static_cast<Entry *>(
        crtAlloc(sizeof(Entry) * (FirstChunkEntries << K)));
    if (!Fresh)
      return nullptr;
    if (cas(&I->Chunks[K], Chunk, Fresh))
      Chunk = Fresh;
    else
      crtFree(Fresh);
  }
  return Chunk ? &Chunk[Slot - FirstChunkEntries * ((1u << K) - 1)] : nullptr;
}

// The sequence word of the record's newest pending entry, which E is set to,
// or zero if it has none.
uint64_t newestEntry(Image *I, Entry *&E) {
  uint32_t Top = load(&I->Top);
  for (uint32_t Slot = Top; Slot;) {
    E = slotEntry(I, Slot - 1);
    uint64_t Seq = load(&E->Seq);
    if (Seq > Entry::Ran)
      return Seq;
    // A slot still being registered stays under Top, with every slot below
    // it, and is only passed over.
    if (Seq != Entry::Ran || Slot != Top) {
      --Slot;
      continue;
    }
    if (cas(&I->Top, Top, Top - 1))
      --Top;
    Slot = Top;
  }
  return 0;
}

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
    void *Free = nullptr;
    if (cas(&I->Dso, Free, Dso))
      return I;
  }
  auto *I = static_cast<Image *>(crtAlloc(sizeof(Image)));
  if (!I)
    return nullptr;
  I->Dso = Dso;
  for (I->Next = load(&R.Images); !cas(&R.Images, I->Next, I);)
    ;
  return I;
}

void pushRetired(Registry &R, Image *I) {
  for (I->NextRetired = load(&R.Retired); !cas(&R.Retired, I->NextRetired, I);)
    ;
}

void retire(ExitState &S, Registry &R, void *Dso) {
  for (Image *I = load(&R.Images); I; I = I->Next) {
    void *Key = Dso;
    if (!cas(&I->Dso, Key, retiredDso()))
      continue;
    I->RetiredIn = __atomic_load_n(&S.Epoch, __ATOMIC_SEQ_CST);
    pushRetired(R, I);
  }
}

// Frees the entries of every record retired two epochs before Current, and
// makes it free; a later one waits. A record is free only once it has been
// reset, so a registration that claims it starts from an empty record.
void release(Registry &R, uint32_t Current) {
  for (Image *I = __atomic_exchange_n(&R.Retired, static_cast<Image *>(nullptr),
                                      __ATOMIC_ACQ_REL),
             *Next;
       I; I = Next) {
    Next = I->NextRetired;
    if (static_cast<int32_t>(Current - I->RetiredIn) < 2) {
      pushRetired(R, I);
      continue;
    }
    for (Entry *&Chunk : I->Chunks) {
      if (Chunk)
        crtFree(Chunk);
      Chunk = nullptr;
    }
    I->Bump = I->Top = 0;
    I->Unloading = 0;
    store(&I->Size, 0);
    store(&I->Dso, nullptr);
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

// The epoch advances once no drainer of the one before it is left, so no
// drainer waits for another, and whoever leaves releases what is due.
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
  bool hold(void *Address) {
    if (reinterpret_cast<uintptr_t>(Address) - Begin < Size)
      return true;
    reset();
    void *Base;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            static_cast<LPCWSTR>(Address), &Module))
      return !RtlPcToFileHeader(Address, &Base);
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

// Runs the pending entries of the records of Dso, or of every live record if
// Dso is null, newest first. The caller is a drainer, whose epoch keeps every
// entry it reads readable. A drain of every record runs an entry only with
// references to the images of its callback and its DSO, taken before it
// claims the entry so that no FreeLibrary can unmap either in between. A
// record whose image refuses one is unloading: the drain marks it, and every
// such drain then leaves it to that image's detach, whose own drain holds the
// loader lock and needs no reference.
void drain(ExitState &S, Registry &R, void *Dso) {
  ModuleReference Code, Data;
  for (;;) {
    // The record whose newest entry is the newest of all, and the newest
    // entry of the others. A registration that this pass misses has yet to
    // advance the sequence for the second time.
    uint64_t Registered = load(&S.Sequence);
    Image *Best = nullptr;
    void *BestKey = nullptr;
    Entry *E = nullptr;
    uint64_t Seq = 0, Others = 0;
    for (Image *I = load(&R.Images); I; I = I->Next) {
      void *Key = load(&I->Dso);
      if (Dso ? Key != Dso : !Key || Key == retiredDso() || load(&I->Unloading))
        continue;
      Entry *Newest;
      uint64_t NewestSeq = newestEntry(I, Newest);
      if (NewestSeq > Seq) {
        Others = Seq;
        Best = I;
        BestKey = Key;
        E = Newest;
        Seq = NewestSeq;
      } else if (NewestSeq > Others) {
        Others = NewestSeq;
      }
    }
    if (!Best)
      return;
    // Stay on the record while its newest entry is the newest of all: until
    // it is older than another record's, or something is registered.
    do {
      Destructor Function = decodeExit(exitCookie(S), E->Dtor);
      if (!Dso && (!Code.hold(reinterpret_cast<void *>(Function)) ||
                   !Data.hold(BestKey))) {
        store(&Best->Unloading, 1);
        break;
      }
      // Another drainer may claim the entry first.
      if (cas(&E->Seq, Seq, Entry::Ran))
        invokeCallback(Function, E->Obj, Seq & 1);
      Seq = newestEntry(Best, E);
    } while (Seq > Others && load(&S.Sequence) == Registered);
  }
}

void drainAll(ExitState &S, Registry &R) {
  uint32_t Entered = enterDrainer(S);
  drain(S, R, nullptr);
  leaveDrainer(S, Entered);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

// Runs before every function in the Universal CRT's atexit table, including
// those another runtime registered after the registries' token, so that the
// exiting thread's thread-local objects are destroyed before any static one.
void NTAPI threadExitCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_DETACH)
    __cxa_thread_finalize(nullptr);
}

void __cdecl normalToken() {
  ExitState &S = exitState();
  LONG Self = static_cast<LONG>(GetCurrentThreadId());
  store(&S.Drained, Self | 2);
  // Nothing is left for it after threadExitCallback, unless the executable
  // could not register that.
  if (__cxa_thread_finalize)
    __cxa_thread_finalize(nullptr);
  __cxa_finalize(nullptr);
  store(&S.Drained, Self | 3);
}

void __cdecl quickToken() {
  ExitState &S = exitState();
  drainAll(S, S.Quick);
}

bool postToken(ExitState &S, Registry &R) {
  if (&R == &S.Quick ? _crt_at_quick_exit(quickToken)
                     : _crt_atexit(normalToken))
    return false;
  store(&R.Token, 1);
  return true;
}

ExitState LocalState = {ExitStateVersion, sizeof(ExitState)};
ExitState *State;

// Returns whether the registry has its token, or needs none. Only the
// executable's start-up posts the token of the process's state, before any
// constructor of the executable runs. The runtime DLL posts the token of a
// state of its own when it is first used, without waiting for a thread that
// is posting it too: both tokens then drain the registry, and the second
// finds nothing left.
bool haveToken(ExitState &S, Registry &R) {
#ifdef COMPILER_RT_SHARED_LIB
  if (&S == &LocalState && !load(&R.Token))
    return postToken(S, R);
#endif
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
  uintptr_t Size = load(&I->Size);
  HMODULE Module;
  if (!Size) {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(Dso), &Module))
      return true;
    Size = imageSize(Module);
    store(&I->Begin, reinterpret_cast<uintptr_t>(Module));
    store(&I->Size, Size);
  }
  uintptr_t Begin = load(&I->Begin);
  return reinterpret_cast<uintptr_t>(Object) - Begin < Size ||
         reinterpret_cast<void *>(Begin) == noDso() ||
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            static_cast<LPCWSTR>(Dso), &Module);
}

// Object is a static object for the C++ registrations, and an adaptor's
// function for the C ones.
int append(Registry ExitState::*Which, Destructor Function, void *Object,
           void *Dso, bool Salted, bool Static) {
  ExitState &S = exitState();
  Registry &R = S.*Which;
  if (!Function || !haveToken(S, R))
    return -1;
  Image *I = claimImage(R, Dso ? Dso : noDso());
  if (!I || (Static && Object && Dso && !keepRegistrar(I, Dso, Object)))
    return -1;
  // A slot is taken only once its entry exists, so that every slot taken is
  // published: one left unpublished would hold Top above the slots below it.
  // The last slots have no entry, so Bump cannot wrap.
  uint32_t Slot = load(&I->Bump);
  Entry *E;
  do {
    E = slotEntry(I, Slot, /*Allocate=*/true);
    if (!E)
      return -1;
  } while (!cas(&I->Bump, Slot, Slot + 1));
  E->Dtor = encodeExit(exitCookie(S), Function);
  E->Obj = Object;
  // The sequence advances for the entry's number and again once a drain can
  // see the entry, so that a drain whose pass over the records missed it
  // finds the sequence changed and looks again.
  uint64_t Number = __atomic_add_fetch(&S.Sequence, 1, __ATOMIC_ACQ_REL);
  store(&E->Seq, Number << 1 | Salted);
  for (uint32_t Top = load(&I->Top);
       Top <= Slot && !cas(&I->Top, Top, Slot + 1);)
    ;
  __atomic_add_fetch(&S.Sequence, 1, __ATOMIC_ACQ_REL);
  return 0;
}

} // namespace

ExitState &wincrt::exitState() {
  ExitState *S = load(&State);
  if (S)
    return *S;
  // The executable's state, if it reserves one and its layout is this copy's:
  // two copies that disagree on it would corrupt each other's lists.
  auto *Base = static_cast<char *>(noDso());
  uint32_t Locator[2];
  __builtin_memcpy(Locator, Base + 0x30, sizeof(Locator));
  S = &LocalState;
  if (Locator[0] == ExitStateTag && Locator[1]) {
    S = reinterpret_cast<ExitState *>(Base + Locator[1]);
    if (S->Version != ExitStateVersion || S->Size != sizeof(ExitState))
      __fastfail(FAST_FAIL_INVALID_IMAGE_BASE);
  }
  store(&State, S);
  return *S;
}

bool wincrt::runQuickExit() {
  ExitState &S = exitState();
  if (&S == &LocalState)
    return false;
  // The C termination is marked complete before the registrations run, as
  // the Universal CRT's quick_exit marks it.
  _c_exit();
  drainAll(S, S.Quick);
  return true;
}

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

int __cxa_at_quick_exit(void (*Function)(void), void *Dso) {
  return Function
             ? append(&ExitState::Quick, reinterpret_cast<Destructor>(callVoid),
                      reinterpret_cast<void *>(Function), Dso, true, false)
             : -1;
}

void __cdecl __cxa_finalize(void *Dso) {
  ExitState &S = exitState();
  uint32_t Entered = enterDrainer(S);
  drain(S, S.Normal, Dso);
  leaveDrainer(S, Entered);
}

// Called by the executable's start-up only, before any of its constructors,
// which may call exit. The Universal CRT accepts one thread-exit callback,
// from the executable.
int __wincrt_register_executable(void) {
  ExitState &S = exitState();
  store(&S.ExecutableRegistered, 1);
  if (__cxa_thread_finalize)
    _register_thread_local_exe_atexit_callback(threadExitCallback);
  return postToken(S, S.Normal) && postToken(S, S.Quick);
}

// FreeLibrary always runs the image's registrations. At process exit under a
// wincrt executable, exit's drain has already run them, or the process ended
// before it finished (_Exit, quick_exit, ExitProcess) and none may run now.
// Under any other host the detach is the image's only termination, as for a
// vcruntime DLL. An unloading image's quick-exit registrations never run.
int __wincrt_detach_image(void *Dso, int Terminating) {
  ExitState &S = exitState();
  if (Terminating && load(&S.ExecutableRegistered) && !(load(&S.Drained) & 1))
    return 0;
  if (__cxa_thread_finalize)
    __cxa_thread_finalize(Dso);
  __cxa_finalize(Dso);
  uint32_t Entered = enterDrainer(S);
  retire(S, S.Normal, Dso);
  retire(S, S.Quick, Dso);
  leaveDrainer(S, Entered);
  return 1;
}

#ifdef COMPILER_RT_SHARED_LIB
// The registries serve the process until it ends: the tokens and the
// thread-exit callback they post must stay mapped until exit runs them, and
// the records outlive the images that registered them. The thread-local
// runtime is founded here, before any image that imports this one
// initializes.
BOOL WINAPI DllMain(HINSTANCE, DWORD Reason, LPVOID) {
  HMODULE Self;
  return Reason != DLL_PROCESS_ATTACH ||
         (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&DllMain), &Self) &&
          wincrt::tls::found());
}
#endif

} // extern "C"
