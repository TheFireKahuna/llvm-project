//===-- cxa_atexit.cpp - Static and quick-exit registries -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One process-wide registry for __cxa_atexit and one for __cxa_at_quick_exit.
// Each registering image owns an arena of entries and a lock-free stack over
// them, so unloading an image frees everything it registered. Every entry
// carries a global sequence number; the process-wide drain merges the image
// stacks by sequence, which yields exactly the order a single list would:
// reverse registration across every image, with entries registered during
// the drain running next.
//
// Nothing here takes a lock. Registration is a sequence increment and a CAS
// push; consumption is a CAS pop shared by the exit drain and by an image's
// own detach, so each entry runs at most once. Stack heads and free-list
// heads are {tag, slot} words, so reusing a slot cannot confuse a concurrent
// pop. Arenas of an unloaded image are freed only when no drainer is active,
// and a thread killed by process termination in the middle of any step leaves
// a consistent registry.
//
// UCRT supplies the order across runtimes: a registry whose image lives for
// the whole process (the shared owner, or a C-only executable's local copy)
// posts one _crt_atexit token when it first registers. UCRT runs that token
// during exit() in reverse order with the host's own atexit handlers, and the
// token drains this registry. Registries inside unloadable C-only DLLs post
// nothing and are drained at their detach instead.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <limits.h>

namespace {

constexpr uint32_t NoSlot = 0xFFFFFFFFu;

// Slots are numbered across geometrically growing chunks: chunk K holds
// FirstChunkEntries << K entries, so 26 chunks address every 32-bit slot.
constexpr uint32_t FirstChunkEntries = 64;
constexpr unsigned MaxChunks = 26;

struct Entry {
  void (*Dtor)(void *);
  void *Obj;
  uint64_t Seq;
  uint32_t Slot;
  uint32_t Next;
};

// {tag, slot} stack head. The tag advances on every successful update.
inline uint64_t makeHead(uint64_t Tag, uint32_t Slot) {
  return (Tag << 32) | Slot;
}
inline uint32_t headSlot(uint64_t Head) { return static_cast<uint32_t>(Head); }
inline uint64_t headTag(uint64_t Head) { return Head >> 32; }

struct Image {
  void *Dso;     // null while the record is unused
  uint64_t Live; // stack of pending entries
  uint64_t Free; // stack of consumed slots
  uint32_t Bump; // next never-used slot
  LONG Retiring; // set by detach once the image is drained
  Entry *Chunks[MaxChunks];
  Image *Next; // global list, append-only
  Image *NextRetired;
};

struct Registry {
  Image *Images;
  Image *Retired;
  LONG Token; // 0 none, 1 posting, 2 posted
};

Registry Normal, Quick;
uint64_t Sequence;
// Registrations without a DSO (Itanium allows a null handle) are keyed on
// this object, since a null Dso marks an unused image record.
char NoDso;
// Drainers hold entries and arenas alive; a negative count means the last
// drainer is freeing retired arenas and newcomers wait for it.
LONG Drainers;
constexpr LONG DrainersFrozen = LONG_MIN / 2;

LONG ExecutableRegistered;
LONG Drained;
void (*AfterDrain)(void);

template <typename T> T load(T *P) {
  return __atomic_load_n(P, __ATOMIC_ACQUIRE);
}
template <typename T> bool cas(T *P, T &Expected, T Desired) {
  return __atomic_compare_exchange_n(P, &Expected, Desired, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Slots and arenas
//===----------------------------------------------------------------------===//

unsigned chunkOf(uint32_t Slot, uint32_t &Offset) {
  uint32_t Scaled = Slot / FirstChunkEntries + 1;
  unsigned K = 31 - __builtin_clz(Scaled);
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
    if (cas(&I->Free, Head, makeHead(headTag(Head) + 1, E->Next)))
      return E;
  }
  uint32_t Slot =
      static_cast<uint32_t>(__atomic_fetch_add(&I->Bump, 1u, __ATOMIC_ACQ_REL));
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
    if (cas(&I->Chunks[K], Expected, Fresh))
      Chunk = Fresh;
    else {
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
    E->Next = headSlot(Old);
    if (cas(Head, Old, makeHead(headTag(Old) + 1, E->Slot)))
      return;
  }
}

// Pops the newest entry, or null. Safe against concurrent pops and slot
// reuse: a stale head fails the CAS through its tag.
Entry *popHead(Image *I, uint64_t *Head) {
  for (uint64_t Old = load(Head);;) {
    uint32_t Slot = headSlot(Old);
    if (Slot == NoSlot)
      return nullptr;
    Entry *E = slotEntry(I, Slot);
    if (cas(Head, Old, makeHead(headTag(Old) + 1, E->Next)))
      return E;
  }
}

//===----------------------------------------------------------------------===//
// Images
//===----------------------------------------------------------------------===//

Image *findImage(Registry &R, void *Dso) {
  for (Image *I = load(&R.Images); I; I = I->Next)
    if (load(&I->Dso) == Dso && !load(&I->Retiring))
      return I;
  return nullptr;
}

Image *claimImage(Registry &R, void *Dso) {
  if (Image *I = findImage(R, Dso))
    return I;
  // Reuse a released record or append a new one. Two threads may create two
  // records for the same image; every per-image operation visits all of them.
  for (Image *I = load(&R.Images); I; I = I->Next) {
    void *Expected = nullptr;
    if (!load(&I->Retiring) && cas(&I->Dso, Expected, Dso))
      return I;
  }
  auto *I = static_cast<Image *>(wincrt::crtAlloc(sizeof(Image)));
  if (!I)
    return nullptr;
  I->Dso = Dso;
  I->Live = makeHead(0, NoSlot);
  I->Free = makeHead(0, NoSlot);
  for (Image *Head = load(&R.Images);;) {
    I->Next = Head;
    if (cas(&R.Images, Head, I))
      return I;
  }
}

// Frees the arenas of every retired image. Runs only with no drainer active.
void flushRetired(Registry &R) {
  for (Image *I = __atomic_exchange_n(&R.Retired, static_cast<Image *>(nullptr),
                                      __ATOMIC_ACQ_REL);
       I;) {
    Image *Next = I->NextRetired;
    for (unsigned K = 0; K < MaxChunks; ++K) {
      wincrt::crtFree(I->Chunks[K]);
      I->Chunks[K] = nullptr;
    }
    I->Live = makeHead(0, NoSlot);
    I->Free = makeHead(0, NoSlot);
    I->Bump = 0;
    __atomic_store_n(&I->Retiring, 0L, __ATOMIC_RELEASE);
    __atomic_store_n(&I->Dso, static_cast<void *>(nullptr), __ATOMIC_RELEASE);
    I = Next;
  }
}

void enterDrainer(bool Terminating) {
  for (;;) {
    LONG Old = load(&Drainers);
    if (Old < 0 && !Terminating) {
      // Another thread is freeing retired arenas; the window is a few frees.
      YieldProcessor();
      continue;
    }
    if (cas(&Drainers, Old, Old + 1))
      return;
  }
}

void leaveDrainer() {
  for (;;) {
    LONG Old = load(&Drainers);
    bool Last = Old == 1 && (load(&Normal.Retired) || load(&Quick.Retired));
    if (!cas(&Drainers, Old, Last ? DrainersFrozen : Old - 1))
      continue;
    if (!Last)
      return;
    flushRetired(Normal);
    flushRetired(Quick);
    __atomic_store_n(&Drainers, 0L, __ATOMIC_RELEASE);
    return;
  }
}

//===----------------------------------------------------------------------===//
// Draining
//===----------------------------------------------------------------------===//

// A counted module reference for the image containing Address, or null for
// addresses outside any image (JIT code, artificial DSO tokens). Held around
// a process-wide callback so a concurrent unload cannot unmap it.
HMODULE reference(const void *Address) {
  HMODULE Module = nullptr;
  if (Address)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       static_cast<LPCWSTR>(Address), &Module);
  return Module;
}

void invoke(void (*Function)(void *), void *Object) {
  wincrt::invokeCallback(Function, Object);
}

void quickInvoke(void *Function) {
  reinterpret_cast<void (*)(void)>(Function)();
}

void run(Registry &R, Image *I, Entry *E, bool Execute) {
  void (*Dtor)(void *) = E->Dtor;
  void *Obj = E->Obj;
  void *Dso = load(&I->Dso);
  pushHead(&I->Free, E);
  if (!Execute)
    return;
  HMODULE Code = reference(&R == &Quick ? Obj : reinterpret_cast<void *>(Dtor));
  HMODULE Data = Dso == &NoDso ? nullptr : reference(Dso);
  invoke(Dtor, Obj);
  if (Data)
    FreeLibrary(Data);
  if (Code)
    FreeLibrary(Code);
}

// Every pending entry across all images, newest first.
void drainAll(Registry &R) {
  enterDrainer(false);
  for (;;) {
    Image *Best = nullptr;
    uint64_t BestSeq = 0;
    for (Image *I = load(&R.Images); I; I = I->Next) {
      if (!load(&I->Dso))
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
      break;
    // A concurrent pop may take the entry first; then this image simply
    // offers its next one on the following round.
    if (Entry *E = popHead(Best, &Best->Live))
      run(R, Best, E, true);
  }
  leaveDrainer();
}

// The pending entries of one image, newest first. Unloading also releases the
// image's arenas once no drainer can still reference them.
void drainImage(Registry &R, void *Dso, bool Execute, bool Unloading,
                bool Terminating) {
  enterDrainer(Terminating);
  for (Image *I = load(&R.Images); I; I = I->Next) {
    if (load(&I->Dso) != Dso || load(&I->Retiring))
      continue;
    while (Entry *E = popHead(I, &I->Live))
      run(R, I, E, Execute);
    if (!Unloading)
      continue;
    LONG Expected = 0;
    if (!cas(&I->Retiring, Expected, 1L))
      continue;
    for (Image *Head = load(&R.Retired);;) {
      I->NextRetired = Head;
      if (cas(&R.Retired, Head, I))
        break;
    }
  }
  leaveDrainer();
}

//===----------------------------------------------------------------------===//
// UCRT tokens
//===----------------------------------------------------------------------===//

void __cdecl normalToken() {
  // The exiting thread's thread-local objects precede every static object.
  __cxa_thread_finalize(nullptr);
  drainAll(Normal);
  __atomic_store_n(&Drained, 1L, __ATOMIC_RELEASE);
  if (void (*Hook)(void) = load(&AfterDrain))
    Hook();
}

void __cdecl quickToken() { drainAll(Quick); }

// Posts the registry's token once. Only a registry whose image outlives the
// process's exit may post: UCRT cannot unregister, so the token must never
// point into unmapped code. The shared owner is retained for that reason; an
// executable is never unloaded; a C-only DLL's local copy posts nothing.
bool postToken(Registry &R) {
  LONG State = load(&R.Token);
  if (State == 2)
    return true;
#ifndef WINCRT_SHARED_CXX_RUNTIME
  if (!load(&ExecutableRegistered))
    return true;
#endif
  LONG Expected = 0;
  if (!cas(&R.Token, Expected, 1L))
    return true;
  void (*Token)(void) = &R == &Quick ? quickToken : normalToken;
#ifdef WINCRT_SHARED_CXX_RUNTIME
  HMODULE Owner = reference(reinterpret_cast<void *>(Token));
#else
  HMODULE Owner = nullptr;
#endif
  int Failed = &R == &Quick ? _crt_at_quick_exit(Token) : _crt_atexit(Token);
  if (Failed) {
    if (Owner)
      FreeLibrary(Owner);
    __atomic_store_n(&R.Token, 0L, __ATOMIC_RELEASE);
    return false;
  }
  __atomic_store_n(&R.Token, 2L, __ATOMIC_RELEASE);
  return true;
}

int append(Registry &R, void (*Function)(void *), void *Object, void *Dso) {
  if (!Function || !postToken(R))
    return -1;
  Image *I = claimImage(R, Dso ? Dso : &NoDso);
  if (!I)
    return -1;
  Entry *E = allocateSlot(I);
  if (!E)
    return -1;
  E->Dtor = Function;
  E->Obj = Object;
  E->Seq = __atomic_add_fetch(&Sequence, 1ull, __ATOMIC_ACQ_REL);
  pushHead(&I->Live, E);
  return 0;
}

} // namespace

extern "C" {

int __cdecl WINCRT_LIFETIME(__cxa_atexit)(void (*Function)(void *),
                                          void *Object, void *Dso) {
  return append(Normal, Function, Object, Dso);
}

void __cdecl WINCRT_LIFETIME(__cxa_finalize)(void *Dso) {
  if (Dso) {
    drainImage(Normal, Dso, true, false, false);
    return;
  }
  drainAll(Normal);
  __atomic_store_n(&Drained, 1L, __ATOMIC_RELEASE);
}

int WINCRT_LIFETIME(__cxa_at_quick_exit)(void (*Function)(void), void *Dso) {
  return append(Quick, quickInvoke, reinterpret_cast<void *>(Function), Dso);
}

// Executable startup: the registry's image is process-lifetime from now on,
// its token may be posted, and Hook runs after the process-wide drain (the
// executable's terminators).
void WINCRT_LIFETIME(__wincrt_register_executable)(void (*Hook)(void)) {
  __atomic_store_n(&AfterDrain, Hook, __ATOMIC_RELEASE);
  __atomic_store_n(&ExecutableRegistered, 1L, __ATOMIC_RELEASE);
  postToken(Normal);
}

// FreeLibrary always finalizes the image. At process exit, a wincrt
// executable's registrations have either been drained by the token, or the
// process terminated abruptly (_Exit, quick_exit, ExitProcess) and no
// destructor may run. Under any other host the image's own detach is the only
// termination signal, as it is for vcruntime DLLs. Quick registrations of an
// unloading image are discarded, never run.
int WINCRT_LIFETIME(__wincrt_detach_image)(void *Dso, int Terminating) {
  if (Terminating && load(&ExecutableRegistered) && !load(&Drained))
    return 0;
  WINCRT_LIFETIME(__cxa_thread_finalize)(Dso);
  drainImage(Normal, Dso, true, true, Terminating != 0);
  drainImage(Quick, Dso, false, true, Terminating != 0);
  return 1;
}

} // extern "C"
