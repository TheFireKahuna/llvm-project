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
// The Universal CRT orders the registry among other runtimes: a registry that
// lives as long as the process (clang_rt.wincrt_dynamic.dll, which pins
// itself, or the copy that -static links into an executable) posts one
// _crt_atexit token when it is first used, and exit runs the token, in
// reverse order with the host's own atexit functions, to drain it. A copy
// that -static links into a DLL posts nothing, since the token cannot be
// withdrawn before the DLL is unloaded, and drains at the DLL's detach
// instead.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <process.h>

using wincrt::Destructor;

// Weak, so that only an image that registers a thread-local destructor links
// their registry.
extern "C" void __cxa_thread_finalize(void *) __attribute__((weak));

namespace {

constexpr uint32_t NoSlot = UINT32_MAX;

// Chunk K of a record holds FirstChunkEntries << K entries, so MaxChunks
// chunks address every 32-bit slot.
constexpr uint32_t FirstChunkEntries = 64;
constexpr unsigned MaxChunks = 26;

struct Entry {
  uintptr_t Dtor; // encoded
  void *Obj;
  // Twice the global sequence number, plus one if Dtor was registered as
  // carrying the salted destructor type.
  uint64_t Seq;
  uint32_t Slot;
  uint32_t Next;
};

// A stack head: the top slot, and a tag that every update advances.
uint64_t makeHead(uint64_t Tag, uint32_t Slot) { return Tag << 32 | Slot; }
uint32_t headSlot(uint64_t Head) { return static_cast<uint32_t>(Head); }
uint64_t headTag(uint64_t Head) { return Head >> 32; }

// The keys of registrations without a DSO handle, which the Itanium ABI
// allows, and of a record that its image's detach retired.
char NoDso;
char RetiredDso;

struct Image {
  // The registering image's handle, &RetiredDso from its detach until the
  // record is released, and null while the record is free.
  void *Dso;
  uint64_t Live; // stack of pending entries
  uint64_t Free; // stack of consumed slots
  uint32_t Bump; // next slot never used
  Entry *Chunks[MaxChunks];
  Image *Next; // every record, append-only
  Image *NextRetired;
  uint32_t RetiredIn; // the drainer generation of its retirement
};

struct Registry {
  Image *Images;
  Image *Hint; // the record found last
  Image *Retired;
  LONG Token;
};

enum : LONG { NoToken, PostingToken, TokenPosted };

Registry Normal, Quick;
uint64_t Sequence;

// Drainers hold records' arenas alive, by epochs. A drainer is counted in
// the epoch it entered, and the epoch advances once no drainer of the one
// before it remains. A drainer that enters after a record's retirement never
// reads the record, so two advances after it no drainer can, and the record
// is released. No drainer ever waits for another.
uint32_t Epoch;
LONG Drainers[2]; // the drainers of the current and the previous epoch

LONG ExecutableRegistered;
LONG Drained;
uintptr_t AfterDrain; // encoded

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
  unsigned K = 31 - __builtin_clz(Slot / FirstChunkEntries + 1);
  Offset = Slot - FirstChunkEntries * ((1u << K) - 1);
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
  if (K >= MaxChunks)
    return nullptr;
  Entry *Chunk = load(&I->Chunks[K]);
  if (!Chunk) {
    auto *Fresh = static_cast<Entry *>(
        wincrt::crtAlloc(sizeof(Entry) * (FirstChunkEntries << K)));
    if (!Fresh)
      return nullptr;
    Entry *Expected = nullptr;
    if (cas(&I->Chunks[K], Expected, Fresh)) {
      Chunk = Fresh;
    } else {
      wincrt::crtFree(Fresh);
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
  auto *I = static_cast<Image *>(wincrt::crtAlloc(sizeof(Image)));
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

void retire(Registry &R, void *Dso) {
  for (Image *I = load(&R.Images); I; I = I->Next) {
    void *Expected = Dso;
    if (!cas(&I->Dso, Expected, static_cast<void *>(&RetiredDso)))
      continue;
    I->RetiredIn = __atomic_load_n(&Epoch, __ATOMIC_SEQ_CST);
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
        wincrt::crtFree(Chunk);
      Chunk = nullptr;
    }
    I->Live = makeHead(headTag(I->Live) + 1, NoSlot);
    I->Free = makeHead(headTag(I->Free) + 1, NoSlot);
    I->Bump = 0;
    store(&I->Dso, static_cast<void *>(nullptr));
    I = Next;
  }
}

// Returns the epoch the caller drains in, to pass to leaveDrainer. A drainer
// counted in an epoch that has already ended counts again in the new one.
uint32_t enterDrainer() {
  for (;;) {
    uint32_t E = __atomic_load_n(&Epoch, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&Drainers[E & 1], 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&Epoch, __ATOMIC_SEQ_CST) == E)
      return E;
    __atomic_sub_fetch(&Drainers[E & 1], 1, __ATOMIC_SEQ_CST);
  }
}

void leaveDrainer(uint32_t Entered) {
  __atomic_sub_fetch(&Drainers[Entered & 1], 1, __ATOMIC_SEQ_CST);
  if (!load(&Normal.Retired) && !load(&Quick.Retired))
    return;
  uint32_t E = __atomic_load_n(&Epoch, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&Drainers[(E + 1) & 1], __ATOMIC_SEQ_CST) == 0 &&
      __atomic_compare_exchange_n(&Epoch, &E, E + 1, false, __ATOMIC_SEQ_CST,
                                  __ATOMIC_SEQ_CST))
    ++E;
  release(Normal, E);
  release(Quick, E);
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

  void hold(const void *Address) {
    auto Value = reinterpret_cast<uintptr_t>(Address);
    if (Value - Begin < Size)
      return;
    reset();
    if (!Address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            static_cast<LPCWSTR>(Address), &Module)) {
      Module = nullptr;
      return;
    }
    Begin = reinterpret_cast<uintptr_t>(Module);
    Size = wincrt::imageSize(Module);
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
// of Dso, or of every live record if Dso is null, newest first. Callbacks
// run with references to their images unless the caller holds the loader
// lock, which already keeps every image mapped. The caller is a drainer.
void drain(Registry &R, void *Dso, bool Execute, bool Reference) {
  ModuleReference Code, Data;
  for (;;) {
    Image *Best = nullptr;
    uint64_t BestSeq = 0;
    for (Image *I = load(&R.Images); I; I = I->Next) {
      void *Key = load(&I->Dso);
      if (Dso ? Key != Dso : !Key || Key == &RetiredDso)
        continue;
      uint32_t Slot = headSlot(load(&I->Live));
      if (Slot == NoSlot)
        continue;
      uint64_t Seq = load(&slotEntry(I, Slot)->Seq);
      if (!Best || Seq > BestSeq) {
        Best = I;
        BestSeq = Seq;
      }
    }
    if (!Best)
      return;
    // A concurrent drainer may pop the entry first; the record then offers
    // its next one on the following round.
    Entry *E = popHead(Best, &Best->Live);
    if (!E)
      continue;
    Destructor Dtor = wincrt::decodePointer(E->Dtor);
    void *Obj = E->Obj;
    bool Salted = E->Seq & 1;
    void *Key = load(&Best->Dso);
    pushHead(&Best->Free, E);
    if (!Execute)
      continue;
    if (Reference) {
      Code.hold(&R == &Quick ? Obj : reinterpret_cast<void *>(Dtor));
      Data.hold(Key == &NoDso ? nullptr : Key);
    }
    wincrt::invokeCallback(Dtor, Obj, Salted);
  }
}

void drainAll(Registry &R) {
  uint32_t Entered = enterDrainer();
  drain(R, nullptr, true, true);
  leaveDrainer(Entered);
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
  // Nothing is left here after threadExitCallback, unless the executable
  // could not register it.
  if (__cxa_thread_finalize)
    __cxa_thread_finalize(nullptr);
  drainAll(Normal);
  store(&Drained, 1L);
  if (uintptr_t Hook = load(&AfterDrain))
    reinterpret_cast<void (*)()>(wincrt::decodePointer(Hook))();
}

void __cdecl quickToken() { drainAll(Quick); }

// Returns whether the registry has its token, or needs none. A registrant
// that finds another posting it waits for the outcome, and posts the token
// itself if that failed, so that no entry is left without one. The poster is
// inside _crt_atexit, which holds the Universal CRT's lock, so a poster that
// never returns has stopped the process's C runtime anyway.
bool postToken(Registry &R) {
  if (load(&R.Token) == TokenPosted)
    return true;
#ifndef COMPILER_RT_SHARED_LIB
  if (!load(&ExecutableRegistered))
    return true;
#endif
  for (LONG Expected = NoToken;
       !cas(&R.Token, Expected, static_cast<LONG>(PostingToken));
       Expected = NoToken) {
    if (Expected == TokenPosted)
      return true;
    YieldProcessor();
  }
  if (&R == &Quick ? _crt_at_quick_exit(quickToken)
                   : _crt_atexit(normalToken)) {
    store(&R.Token, static_cast<LONG>(NoToken));
    return false;
  }
  store(&R.Token, static_cast<LONG>(TokenPosted));
  return true;
}

int append(Registry &R, Destructor Function, void *Object, void *Dso,
           bool Salted) {
  if (!Function || !postToken(R))
    return -1;
  Image *I = claimImage(R, Dso ? Dso : &NoDso);
  if (!I)
    return -1;
  Entry *E = allocateSlot(I);
  if (!E)
    return -1;
  E->Dtor = wincrt::encodePointer(Function);
  E->Obj = Object;
  __atomic_store_n(&E->Seq,
                   __atomic_add_fetch(&Sequence, 2, __ATOMIC_RELAXED) | Salted,
                   __ATOMIC_RELAXED);
  pushHead(&I->Live, E);
  return 0;
}

} // namespace

extern "C" {

int __cdecl __cxa_atexit(void (*Function)(void *), void *Object, void *Dso) {
  return append(Normal, Function, Object, Dso, false);
}

// Clang registers its destructors here, since they carry the salted type.
int __cdecl __llvm_kcfi_cxa_atexit(void (*Function)(void *), void *Object,
                                   void *Dso) {
  return append(Normal, Function, Object, Dso, true);
}

void __cdecl __cxa_finalize(void *Dso) {
  if (!Dso) {
    drainAll(Normal);
    store(&Drained, 1L);
    return;
  }
  uint32_t Entered = enterDrainer();
  drain(Normal, Dso, true, true);
  leaveDrainer(Entered);
}

int __cxa_at_quick_exit(void (*Function)(void), void *Dso) {
  if (!Function)
    return -1;
  return append(Quick, reinterpret_cast<Destructor>(wincrt::callVoid),
                reinterpret_cast<void *>(Function), Dso, true);
}

// Called by the executable's start-up only, and before any constructor,
// which may call exit. The Universal CRT accepts one thread-exit callback,
// from the executable.
void __wincrt_register_executable(void (*Hook)(void)) {
  store(&AfterDrain, wincrt::encodePointer(reinterpret_cast<Destructor>(Hook)));
  store(&ExecutableRegistered, 1L);
  if (__cxa_thread_finalize)
    _register_thread_local_exe_atexit_callback(threadExitCallback);
  postToken(Normal);
}

// FreeLibrary always runs the image's registrations. At process exit under a
// wincrt executable, exit's token has already run them, or the process ended
// without running any (_Exit, quick_exit, ExitProcess) and none may run now.
// Under any other host the detach is the image's only termination, as for a
// vcruntime DLL. An unloading image's quick-exit registrations never run.
int __wincrt_detach_image(void *Dso, int Terminating) {
  if (Terminating && load(&ExecutableRegistered) && !load(&Drained))
    return 0;
  if (__cxa_thread_finalize)
    __cxa_thread_finalize(Dso);
  // The loader lock is held, so no reference is needed.
  uint32_t Entered = enterDrainer();
  drain(Normal, Dso, true, false);
  drain(Quick, Dso, false, false);
  retire(Normal, Dso);
  retire(Quick, Dso);
  leaveDrainer(Entered);
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
