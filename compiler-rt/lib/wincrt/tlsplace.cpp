//===-- tlsplace.cpp - Images' thread-locals and their places -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An image's fill registers it: registration places the image's thread-locals
// in 16-byte granules of an area every thread has at one offset from its
// thread pointer, adds the image to the set, and writes the image's template
// into every thread the runtime holds before the fill publishes the place, so
// that no thread runs the image's code before its bytes are there. A thread
// adopted later is seeded from every image the set holds, again while a
// registration's generation moves under it.
//
// The set holds bases only: every use reads the image's .tls header and its
// place word, the first value of its .sealed section, from the image itself.
//
//===----------------------------------------------------------------------===//

#include "tlsrt.h"

using namespace wincrt::tls;

namespace {

// An image of the model: its template, the read-only .tls section, of Size
// bytes aligned to Align, and its place word, where it has one.
struct Image {
  char *Base;
  const char *Tls;
  uint32_t Size, Align;
  uint64_t *Place;
};

// Reads an image's headers by probe, from a copy of their page: a set entry
// may name an image another thread is unloading, which reads as absent.
bool readImage(char *Base, Image &I) {
  uint64_t Headers[Page / 8];
  I = {Base, nullptr, 0, 0, nullptr};
  if (!__wincrt_probe_copy(Headers, Base, Page / 8))
    return false;
  auto *Bytes = reinterpret_cast<char *>(Headers);
  uint32_t At = reinterpret_cast<IMAGE_DOS_HEADER *>(Bytes)->e_lfanew;
  if (At > Page - sizeof(IMAGE_NT_HEADERS))
    return false;
  auto *Nt = reinterpret_cast<IMAGE_NT_HEADERS *>(Bytes + At);
  unsigned N = Nt->FileHeader.NumberOfSections;
  IMAGE_SECTION_HEADER *Section = IMAGE_FIRST_SECTION(Nt);
  if (reinterpret_cast<char *>(Section + N) > Bytes + Page)
    return false;
  for (; N--; ++Section) {
    char *Data = Base + Section->VirtualAddress;
    if (!__builtin_memcmp(Section->Name, ".tls\0\0\0", 8) &&
        !(Section->Characteristics & IMAGE_SCN_MEM_WRITE)) {
      unsigned Shift = Section->Characteristics >> 20 & 15;
      I.Tls = Data;
      I.Size = Section->Misc.VirtualSize;
      I.Align = Shift ? 1u << (Shift - 1) : 16;
    } else if (!__builtin_memcmp(Section->Name, ".sealed", 8) &&
               Section->Misc.VirtualSize >=
                   TP_PLACE_IN_SEALED + TP_PLACE_WORD_SIZE) {
      I.Place = reinterpret_cast<uint64_t *>(Data + TP_PLACE_IN_SEALED);
    }
  }
  return I.Tls && I.Align <= TP_BLOCK;
}

bool isWide(const Image &I) { return I.Size > TP_BLOCK; }
unsigned granules(const Image &I) {
  return I.Size ? (I.Size + Granule - 1) / Granule : 1;
}

char *executable() {
  return static_cast<char *>(
      NtCurrentTeb()->ProcessEnvironmentBlock->Reserved3[1]);
}

uint64_t run(unsigned N) {
  return N >= 64 ? ~uint64_t(0) : (uint64_t(1) << N) - 1;
}
uint64_t probeLoad(const uint64_t *W) { return __wincrt_probe64(W); }

//===----------------------------------------------------------------------===//
// The set
//===----------------------------------------------------------------------===//

// An entry: an image, by its base.
struct Entry {
  uint64_t Base;
  uint64_t Word;
};
enum : uint64_t { EntryLive = 1, StateMask = 3 };

struct SetPage {
  SetPage *Next;
  uint64_t Pad;
  Entry Entries[(Page - 16) / sizeof(Entry)];
};
SetPage **first() { return reinterpret_cast<SetPage **>(&shared().Set); }

// Every entry in use, which Each may stop the walk at by answering true.
template <typename F> bool eachEntry(F Each) {
  for (SetPage *P = load(first()); P; P = load(&P->Next))
    for (Entry &E : P->Entries)
      if (uint64_t W = load(&E.Word))
        if (Each(E, W))
          return true;
  return false;
}

// Takes a free entry for Base by one compare-exchange, growing the set by a
// page where it has none.
Entry *admit(uint64_t Base) {
  for (SetPage **Link = first();;) {
    SetPage *P = load(Link);
    if (!P) {
      void *Fresh = nullptr;
      SIZE_T Size = Page;
      if (NtAllocateVirtualMemoryEx(GetCurrentProcess(), &Fresh, &Size,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                    nullptr, 0) < 0)
        return nullptr;
      if (!cas(Link, P, static_cast<SetPage *>(Fresh))) {
        Size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), &Fresh, &Size, MEM_RELEASE);
      }
      continue;
    }
    for (Entry &E : P->Entries) {
      uint64_t Free = 0;
      if (cas(&E.Base, Free, Base))
        return &E;
    }
    Link = &P->Next;
  }
}

void leave(Entry &E) {
  store(&E.Word, 0);
  store(&E.Base, 0);
}

// The live entry of the image at Base: a removed one's may still name the
// same base, where another image is mapped since.
Entry *find(char *Base) {
  Entry *Found = nullptr;
  eachEntry([&](Entry &E, uint64_t W) {
    return (W & StateMask) == EntryLive && load(&E.Base) == uint64_t(Base) &&
           (Found = &E);
  });
  return Found;
}

//===----------------------------------------------------------------------===//
// Areas
//===----------------------------------------------------------------------===//

// An area: a kilobyte of every thread's, at Offset from its thread pointer,
// in 16-byte granules, of which Taken has a bit for each taken one. The areas
// are the block, whose header and wide area are never placed, and planes 1
// and 2, which their first take opens; planes 3 and 4 hold no image.
struct Area {
  uint64_t *Taken;
  uint64_t Offset;
  unsigned Plane;
};
constexpr unsigned Fixed = 3;
Area fixedArea(unsigned I) {
  return {&shared().Taken[I], I * uint64_t(TP_PLANE), I};
}

// Opens or closes plane P: one change of the plane word stands at a time;
// the run is committed in, or returned from, every chunk the change reads
// live or reviving, or parked for a close, as a revival commits what it reads
// open after its own state's change. A chunk parked meanwhile is returned
// once the change has ended.
bool change(unsigned P, bool Open) {
  uint64_t Planes = load(&shared().Planes);
  if (Planes & Change ||
      !cas(&shared().Planes, Planes, (Planes ^ uint64_t(1) << P) | Change))
    return false;
  bool Done = true;
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C) {
    uint64_t State = peek(&chunkAt(C).Word) & ChunkState;
    if (Open && (State == ChunkLive || State == ChunkReviving))
      Done = Done && commit(planeRun(C, P), TP_PLANE);
    else if (!Open && State != ChunkEmpty && State != ChunkParking)
      decommit(planeRun(C, P), TP_PLANE);
  }
  __atomic_fetch_and(&shared().Planes, ~Change, __ATOMIC_ACQ_REL);
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C)
    if ((load(&chunkAt(C).Word) & ChunkState) == ChunkParked)
      park(C);
  if (!Done)
    change(P, false);
  return Done;
}

// A placement: its offset from the thread pointer, and its area, whose
// bitmap is null where nothing was placed.
struct Placed {
  uint64_t Place;
  Area In;
};

// Takes N granules at a multiple of Step from area A by one compare-exchange
// of its bitmap; the first take of a plane opens it.
Placed takeIn(Area A, unsigned N, unsigned Step) {
  for (uint64_t Taken = load(A.Taken);;) {
    unsigned G = 0;
    while (G + N <= 64 && Taken & run(N) << G)
      G += Step;
    if (G + N > 64)
      return {};
    if (!cas(A.Taken, Taken, Taken | run(N) << G))
      continue;
    if (!Taken && A.Plane && !change(A.Plane, true)) {
      __atomic_fetch_and(A.Taken, ~(run(N) << G), __ATOMIC_ACQ_REL);
      return {};
    }
    return {A.Offset + G * Granule, A};
  }
}

// Places N granules at a multiple of Step: the block, then plane 1 and
// plane 2.
Placed place(unsigned N, unsigned Step) {
  for (unsigned I = 0; I != Fixed; ++I)
    if (Placed P = takeIn(fixedArea(I), N, Step); P.In.Taken)
      return P;
  return {};
}

Placed placedAt(uint64_t Place) {
  for (unsigned I = 0; I != Fixed; ++I)
    if (Place - fixedArea(I).Offset < TP_BLOCK)
      return {Place, fixedArea(I)};
  return {};
}

// Returns a placement's granules, closing the plane it leaves empty.
void givePlace(Placed P, unsigned N) {
  uint64_t Mask = run(N) << (P.Place - P.In.Offset) / Granule;
  if (!(__atomic_fetch_and(P.In.Taken, ~Mask, __ATOMIC_ACQ_REL) & ~Mask) &&
      P.In.Plane)
    change(P.In.Plane, false);
}

// The executable's place, which its link fixed: past the wide area in the
// block, or plane 1 whole.
uint64_t executablePlace(const Image &I) {
  return I.Size <= TP_BLOCK - TP_EXE_PLACE ? TP_EXE_PLACE
                                                   : TP_PLANE;
}

uint64_t placeOf(const Image &I) {
  return I.Base == executable() ? executablePlace(I)
         : I.Place              ? probeLoad(I.Place)
                                : 0;
}

//===----------------------------------------------------------------------===//
// Installation
//===----------------------------------------------------------------------===//

enum How { Seed, Walk, Again };

// Writes image I's template into the thread of record Index: the whole range
// a seed or a walk; again, each word that the thread still holds unfilled.
// The template is read by probe, as its image may be unloading.
void install(const Image &I, uint64_t Place, uint32_t Index, How H) {
  // A thread that has begun its exit runs no more code of the model.
  if (H != Seed && stateOf(probeLoad(&recordAt(Index).Word)) == Releasing)
    return;
  auto *To = reinterpret_cast<uint64_t *>(blockAt(Index) + Place);
  auto *From = reinterpret_cast<const uint64_t *>(I.Tls);
  size_t Words = granules(I) * Granule / 8;
  if (H != Again)
    __wincrt_probe_copy(To, From, Words);
  else
    for (uint64_t W = 0, Value; W != Words; ++W)
      if (__wincrt_probe_copy(&Value, &From[W], 1) && Value != TP_UNFILLED)
        __wincrt_probe_cas(&To[W], TP_UNFILLED, Value);
}

// Every claimed slot of a live chunk, by the chunk's bits.
template <typename F> void eachRecord(F Each) {
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C) {
    uint64_t W = load(&chunkAt(C).Word);
    if ((W & ChunkState) == ChunkLive)
      for (uint64_t Taken = ~W & FullBits; Taken; Taken &= Taken - 1)
        Each(C * 64 + __builtin_ctzll(Taken));
  }
}

// Each registered image, and its place: an image that its fill has not
// published, or that is unmapped under the read, has none and is passed over,
// since no place is 0 or unfilled.
template <typename F> void eachImage(F Each) {
  eachEntry([&](Entry &E, uint64_t W) {
    Image I;
    if ((W & StateMask) == EntryLive &&
        readImage(reinterpret_cast<char *>(load(&E.Base)), I))
      if (uint64_t Place = placeOf(I); Place && Place != TP_UNFILLED)
        Each(I, Place);
    return false;
  });
}

} // namespace

void wincrt::tls::seedImages(uint32_t Index) {
  eachImage([&](Image &I, uint64_t Place) { install(I, Place, Index, Seed); });
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

// Places the image at Base and writes its template into every thread, or
// answers its place again where the set holds it, writing each word a thread
// holds unfilled. Answers 0 for a refusal.
static uint64_t registerImage(char *Base) {
  Image I;
  if (!readImage(Base, I) || !header(true) || isWide(I) ||
      (I.Place && load(&I.Place[1]) != TP_FORM_DIRECT))
    return 0;
  if (find(Base)) {
    uint64_t Place = placeOf(I);
    add(&shared().CreateGen, 1);
    eachRecord([&](uint32_t Index) { install(I, Place, Index, Again); });
    return Place;
  }
  Placed P = place(granules(I), (I.Align + Granule - 1) / Granule);
  Entry *E = P.In.Taken ? admit(uint64_t(Base)) : nullptr;
  if (!E) {
    if (P.In.Taken)
      givePlace(P, granules(I));
    return 0;
  }
  store(&E->Word, EntryLive);
  add(&shared().CreateGen, 1);
  eachRecord([&](uint32_t Index) { install(I, P.Place, Index, Walk); });
  return P.Place;
}

// An unloaded image's granules return, closing a plane it leaves empty, and
// its base leaves the set. Its code is gone, so no thread reads its
// thread-locals again.
static void unloadImage(char *Base) {
  Entry *E = find(Base);
  Image I;
  if (!E || !readImage(Base, I))
    return;
  if (Placed P = placedAt(placeOf(I)); P.In.Taken)
    givePlace(P, granules(I));
  leave(*E);
}

namespace {
struct DllNotification {
  ULONG Flags;
  const UNICODE_STRING *FullName, *BaseName;
  void *Base;
  ULONG Size;
};
} // namespace

// The loader notifies an unload after the image's detach and before it is
// unmapped, so its headers are still there to read.
static void CALLBACK notified(ULONG Reason, const DllNotification *Data,
                              void *) {
  if (Reason == 2)
    unloadImage(static_cast<char *>(Data->Base));
}

// The executable is registered at founding with its template as linked, its
// place the link's; its fill registers it again. The loader's notification
// is found by name, as its documentation has it, and has no KCFI type.
__attribute__((no_sanitize("kcfi"))) bool wincrt::tls::foundImages() {
  using Register = NTSTATUS(NTAPI *)(
      ULONG, void(CALLBACK *)(ULONG, const DllNotification *, void *), void *,
      void **);
  auto Notify = reinterpret_cast<Register>(GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
  void *Cookie;
  if (!Notify || Notify(0, notified, nullptr, &Cookie) < 0)
    return false;
  Image I;
  if (!readImage(executable(), I))
    return true;
  if (isWide(I))
    return false;
  if (executablePlace(I) == TP_PLANE)
    shared().Taken[1] = ~uint64_t(0), shared().Planes |= uint64_t(1) << 1;
  else
    shared().Taken[0] |= run(granules(I)) << TP_EXE_PLACE / Granule;
  Entry *E = admit(uint64_t(executable()));
  return E && (store(&E->Word, EntryLive), true);
}

extern "C" WINCRT_TLS_API WINCRT_NO_TP uint64_t __tp_register(void *Base) {
  return registerImage(static_cast<char *>(Base));
}
