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
// A wide image, of more than a kilobyte, has a word in place of its storage:
// the address of the thread's slot in the image's arena for the thread's
// chunk, encoded with the process's key. Where the block and planes 1 and 2
// have no room, an overflow region gives every thread five kilobytes more, at
// one offset from its thread pointer, in positions placed as planes are.
//
// The set holds bases only: every use reads the image's .tls header and its
// place word, the first value of its .sealed section, from the image itself.
// Every step that reads its wide or overflow entries and acts on them does so
// under its hold, and the address space of a removed image's arenas, or of a
// closed region, is released once no hold older than the removal stands.
//
//===----------------------------------------------------------------------===//

#include "tlsrt.h"

using namespace wincrt::tls;

extern "C" errno_t __cdecl __wrap_rand_s(unsigned *);

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
// A wide image's slot: its size rounded up to 64 bytes, or to a page where it
// is a page or more, which a slot then commits and decommits on its own.
uintptr_t slotSize(const Image &I) {
  uintptr_t Round = I.Size >= Page ? Page : 64;
  return (I.Size + Round - 1) & ~(Round - 1);
}
bool paged(const Image &I) { return isWide(I) && slotSize(I) >= Page; }
uintptr_t arenaSize(const Image &I) {
  return (48 * slotSize(I) + Page - 1) & ~(Page - 1);
}

//===----------------------------------------------------------------------===//
// The set
//===----------------------------------------------------------------------===//

// An entry: an image, by its base, or an overflow region, by its encoded
// base. Word holds the kind, the state, a region's five positions' states and
// the era a removal or a close stamped; Taken each position's granules, all
// set while it is closed, and a removed wide image's place.
struct Entry {
  uint64_t Base;
  uint64_t Word;
  uint64_t Taken[5];
  uint64_t Pad;
};
enum : uint64_t {
  // An image's states, and a region's: live, removing or closing, released
  // by a scan.
  EntryLive = 1,
  Removing = 2,
  Closing = 2,
  Released = 3,
  StateMask = 3,
  // A position's: closed, opening, open, closing.
  PosOpening = 1,
  PosOpen = 2,
  PosClosing = 3,
  EraShift = 12,
  WideEntry = uint64_t(1) << 62,
  RegionEntry = uint64_t(1) << 63,
};
uint64_t positionOf(uint64_t W, unsigned P) { return W >> (2 + 2 * P) & 3; }
uint64_t withPosition(uint64_t W, unsigned P, uint64_t To) {
  return (W & ~(uint64_t(3) << (2 + 2 * P))) | To << (2 + 2 * P);
}
uint64_t eraOf(uint64_t W) { return W << 2 >> (2 + EraShift); }

struct SetPage {
  SetPage *Next;
  uint64_t Pad[7];
  Entry Entries[(Page - 64) / sizeof(Entry)];
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
    return !(W & RegionEntry) && (W & StateMask) == EntryLive &&
           load(&E.Base) == uint64_t(Base) && (Found = &E);
  });
  return Found;
}

//===----------------------------------------------------------------------===//
// Holds and grace
//===----------------------------------------------------------------------===//

// A step that reads the set's wide or overflow entries and acts on them holds
// the era it began in, so that a removal stamped later waits for it. A
// process with neither takes no hold.
bool hold(uint64_t &H) {
  if (!load(&shared().Holders))
    return false;
  __atomic_exchange_n(&H, load(&shared().Era), __ATOMIC_ACQ_REL);
  return true;
}

void scan(Journal &J);

void unhold(uint64_t &H, Journal &J) {
  if (__atomic_exchange_n(&H, 0, __ATOMIC_ACQ_REL) && load(&shared().Pending))
    scan(J);
}

// Stamps an entry removing or closing: first past every era, then at the era
// its own advance makes, so that a step holding an older era may still act on
// the entry and every later one reads it stamped, however stamps interleave.
bool stamp(Entry &E, uint64_t W, uint64_t To) {
  constexpr uint64_t Eras = ~uint64_t(0) >> 14 << EraShift;
  uint64_t Provisional = (To & ~Eras) | Eras;
  add(&shared().Pending, 1);
  if (!cas(&E.Word, W, Provisional)) {
    add(&shared().Pending, -1);
    return false;
  }
  uint64_t Era = add(&shared().Era, 1) + 1;
  cas(&E.Word, Provisional, (To & ~Eras) | Era << EraShift);
  return true;
}

//===----------------------------------------------------------------------===//
// The journal
//===----------------------------------------------------------------------===//

// A wide word and an arena's home hold an address encoded with the key.
uint64_t encode(uint64_t Address) {
  return __builtin_rotateleft64(Address ^ *load(&shared().Key), TP_ROTATE);
}
uint64_t decode(uint64_t Word) {
  return __builtin_rotateright64(Word, TP_ROTATE) ^ *load(&shared().Key);
}

// A reservation of Size made into J above Floor, where the kernel grants one
// there, for Home.
bool make(Journal &J, uint64_t *Home, uintptr_t Size, uintptr_t Floor) {
  MEM_ADDRESS_REQUIREMENTS Requirements = {reinterpret_cast<void *>(Floor)};
  MEM_EXTENDED_PARAMETER Above = {};
  Above.Type = MemExtendedParameterAddressRequirements;
  Above.Pointer = &Requirements;
  J.Home = Home;
  for (unsigned Try = Floor ? 0 : 1; Try != 2; ++Try) {
    J.Making = {nullptr, Size};
    if (NtAllocateVirtualMemoryEx(GetCurrentProcess(), &J.Making.Base,
                                  &J.Making.Size, MEM_RESERVE, PAGE_READWRITE,
                                  Try ? nullptr : &Above, !Try) >= 0)
      return true;
  }
  J.Making = {};
  return false;
}

// The size is zero until the kernel writes back what it released, so that a
// finisher tells a release still owed from one done.
void unreserve(Journal &J, void *Base) {
  J.Freeing = {Base, 0};
  NtFreeVirtualMemory(GetCurrentProcess(), &J.Freeing.Base, &J.Freeing.Size,
                      MEM_RELEASE);
  store(&J.Freeing.Base, nullptr);
}

// Publishes J's reservation, encoded, into its home, which holds From, or
// releases it where another's is there.
bool publish(Journal &J, uint64_t From = 0) {
  uint64_t Encoded = encode(uint64_t(J.Making.Base));
  bool Won = __wincrt_probe_cas(J.Home, From, Encoded) == From &&
             probeLoad(J.Home) == Encoded;
  if (!Won)
    unreserve(J, J.Making.Base);
  store(&J.Making.Base, nullptr);
  return Won;
}

//===----------------------------------------------------------------------===//
// Areas
//===----------------------------------------------------------------------===//

// Each region's base, and its offset from the block area modulo 2^64.
uint64_t regionBase(Entry &E) { return decode(load(&E.Base)); }
char *regionRun(Entry &E, uint32_t C, unsigned P) {
  return reinterpret_cast<char *>(regionBase(E)) + (uintptr_t(C) << 18) +
         16 * TP_BLOCK + P * TP_PLANE;
}

// An area: storage of every thread's at Offset from its thread pointer, in
// granules of 1 << Shift bytes, of which Taken has a bit for each taken one.
// Fixed areas are the wide area, whose words beyond its eight are never
// free; the block, whose header and wide area are never placed; and planes
// 1 and 2, which their first take opens; planes 3 and 4 hold no image. Every
// other is an overflow region's position, whose closed bitmap is full.
struct Area {
  uint64_t *Taken;
  uint64_t Offset;
  unsigned Shift, Plane;
  Entry *Region;
};
constexpr unsigned Fixed = 4;
Area fixedArea(unsigned I) {
  return I ? Area{&shared().Taken[I - 1], (I - 1) * uint64_t(TP_PLANE), 4,
                  I - 1, nullptr}
           : Area{&shared().Wide, TP_H, 3, 0, nullptr};
}
Area positionArea(Entry &E, unsigned P) {
  return {&E.Taken[P], regionBase(E) - uint64_t(area()) + P * TP_PLANE, 4, P,
          &E};
}

// Opens or closes plane P, or region E's position P: one change of the plane
// word stands at a time; the run is committed in, or returned from, every
// chunk the change reads live or reviving, or parked for a close, as a
// revival commits what it reads open after its own state's change. A chunk
// parked meanwhile is returned once the change has ended.
bool change(unsigned P, Entry *E, bool Open) {
  uint64_t Planes = load(&shared().Planes);
  uint64_t Bit = E ? 0 : uint64_t(1) << P;
  if (Planes & Change ||
      !cas(&shared().Planes, Planes, (Planes ^ Bit) | Change))
    return false;
  bool Done = true;
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C) {
    uint64_t State = peek(&chunkAt(C).Word) & ChunkState;
    void *Run = E ? regionRun(*E, C, P) : planeRun(C, P);
    if (Open && (State == ChunkLive || State == ChunkReviving))
      Done = Done && commit(Run, TP_PLANE);
    else if (!Open && State != ChunkEmpty && State != ChunkParking)
      decommit(Run, TP_PLANE);
  }
  clearBits(&shared().Planes, Change);
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C)
    if ((load(&chunkAt(C).Word) & ChunkState) == ChunkParked)
      park(C);
  if (!Done)
    change(P, E, false);
  return Done;
}

// Opens region E's position P: its state from closed to opening, its run
// committed in every live chunk, its bitmap from closed to empty, and the
// state to open, the position's linearization point.
bool openPosition(Entry &E, unsigned P) {
  uint64_t W = load(&E.Word), Closed = ~uint64_t(0);
  if ((W & StateMask) != EntryLive || positionOf(W, P) ||
      !cas(&E.Word, W, withPosition(W, P, PosOpening)))
    return false;
  bool Opened = change(P, &E, true) && cas(&E.Taken[P], Closed, 0);
  for (W = load(&E.Word);
       !cas(&E.Word, W, withPosition(W, P, Opened ? PosOpen : 0));)
    ;
  return Opened;
}

// Closes a position its last granule left: its bitmap from empty to closed,
// the close's linearization point, its run returned in every chunk, then its
// state closed; where that closes the region's last, the region is stamped
// closing, and a scan releases it once no older hold stands.
void closePosition(Entry &E, unsigned P, Journal &J) {
  uint64_t Empty = 0, W;
  if (!cas(&E.Taken[P], Empty, ~uint64_t(0)))
    return;
  for (W = load(&E.Word); !cas(&E.Word, W, withPosition(W, P, PosClosing));)
    ;
  change(P, &E, false);
  for (W = load(&E.Word);; W = load(&E.Word)) {
    uint64_t To = withPosition(W, P, 0);
    if (To >> 2 & 0x3FF ? cas(&E.Word, W, To)
                        : stamp(E, W, (To & ~StateMask) | Closing))
      break;
  }
  scan(J);
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
    if (!Taken && !A.Region && A.Plane && !change(A.Plane, nullptr, true)) {
      clearBits(A.Taken, run(N) << G);
      return {};
    }
    return {A.Offset + (G << A.Shift), A};
  }
}

// Reserves a new region, the block area's span, above a floor drawn at
// random, or anywhere the kernel refuses that, into the caller's journal,
// published by a compare-exchange of its base into a set entry.
Entry *newRegion(Journal &J) {
  Entry *E = admit(1);
  unsigned Draw[2] = {};
  __wrap_rand_s(&Draw[0]);
  __wrap_rand_s(&Draw[1]);
  uintptr_t Floor =
      (uint64_t(Draw[1]) << 32 | Draw[0]) % 0x7F0000000000 & ~uintptr_t(0xFFFF);
  if (!E || !make(J, &E->Base, uintptr_t(Chunks) * ChunkSize, Floor)) {
    if (E)
      leave(*E);
    return nullptr;
  }
  for (unsigned P = 0; P != 5; ++P)
    E->Taken[P] = ~uint64_t(0);
  if (!publish(J, 1))
    return nullptr;
  add(&shared().Holders, 1);
  store(&E->Word, RegionEntry | EntryLive);
  return E;
}

// Places N granules at a multiple of Step: a wide image's word in the wide
// area first; then the block, plane 1 and plane 2, the open positions of open
// regions, a closed position of an open region, a closing region taken back,
// and only then a new region.
Placed place(unsigned N, unsigned Step, bool Wide, Journal &J) {
  for (unsigned I = !Wide; I != Fixed; ++I)
    if (Placed P = takeIn(fixedArea(I), N, Step); P.In.Taken)
      return P;
  Placed Found = {};
  auto Open = [&](Entry &E, unsigned P) {
    return (Found = takeIn(positionArea(E, P), N, Step)).In.Taken;
  };
  for (unsigned Pass = 0; Pass != 3 && !Found.In.Taken; ++Pass)
    eachEntry([&](Entry &E, uint64_t W) {
      if (!(W & RegionEntry))
        return false;
      if (Pass == 2) {
        if ((W & StateMask) != Closing ||
            !cas(&E.Word, W, (W & ~StateMask) | EntryLive))
          return false;
        add(&shared().Pending, -1);
        return openPosition(E, 0) && Open(E, 0);
      }
      for (unsigned P = 0; P != 5; ++P)
        if ((Pass ? openPosition(E, P)
                  : (W & StateMask) == EntryLive &&
                        positionOf(W, P) == PosOpen) &&
            Open(E, P))
          return true;
      return false;
    });
  if (Entry *E = Found.In.Taken ? nullptr : newRegion(J);
      E && openPosition(*E, 0))
    Open(*E, 0);
  return Found;
}

Placed placedAt(uint64_t Place) {
  for (unsigned I = 0; I != Fixed; ++I)
    if (Area A = fixedArea(I);
        Place - A.Offset < (I ? TP_BLOCK : 8 * TP_WIDE_WORDS))
      return {Place, A};
  Placed Found = {};
  eachEntry([&](Entry &E, uint64_t W) {
    for (unsigned P = 0; W & RegionEntry && P != 5; ++P)
      if (Place - positionArea(E, P).Offset < TP_BLOCK)
        return (Found = {Place, positionArea(E, P)}), true;
    return false;
  });
  return Found;
}

// Returns a placement's granules, closing the plane or the position it
// leaves empty.
void givePlace(Placed P, unsigned N, Journal &J) {
  uint64_t Mask = run(N) << ((P.Place - P.In.Offset) >> P.In.Shift);
  if (clearBits(P.In.Taken, Mask) & ~Mask)
    return;
  if (P.In.Region)
    closePosition(*P.In.Region, P.In.Plane, J);
  else if (P.In.Plane)
    change(P.In.Plane, nullptr, false);
}

//===----------------------------------------------------------------------===//
// Wide words
//===----------------------------------------------------------------------===//

// The executable's place, which its link fixed: past the wide area in the
// block, plane 1 whole, or wide, its word the wide area's first.
uint64_t executablePlace(const Image &I) {
  return isWide(I)                           ? TP_H
         : I.Size <= TP_BLOCK - TP_EXE_PLACE ? TP_EXE_PLACE
                                             : TP_PLANE;
}

uint64_t placeOf(const Image &I) {
  return I.Base == executable() ? executablePlace(I)
         : I.Place              ? probeLoad(I.Place)
                                : 0;
}

// The home of chunk C's arena of the wide image whose word is at Place: the
// cell at Place in the chunk's slot 63, whose block is never claimed, and
// which is committed exactly while the area that holds Place is.
uint64_t *homeOf(uint32_t C, uint64_t Place) {
  return reinterpret_cast<uint64_t *>(blockAt(C * 64 + 63) + Place);
}

// Makes chunk C's arena of a wide image where its home holds none: a
// reservation of 48 slots, published, and then, for slots under a page,
// committed.
bool makeArena(uint32_t C, const Image &I, uint64_t Place, Journal &J) {
  uint64_t *Home = homeOf(C, Place);
  if (probeLoad(Home))
    return true;
  if (!make(J, Home, arenaSize(I), 0))
    return false;
  void *Arena = J.Making.Base;
  if (!publish(J))
    return probeLoad(Home) ||
           (load(&chunkAt(C).Word) & ChunkState) != ChunkLive;
  return paged(I) || commit(Arena, arenaSize(I));
}

// Releases chunk C's arena of a wide image, whichever of the chunk's park and
// a removal's scan takes it from its home.
void releaseArena(uint32_t C, uint64_t Place, Journal &J) {
  uint64_t *Home = homeOf(C, Place), Was = probeLoad(Home);
  if (Was && __wincrt_probe_cas(Home, Was, 0) == Was)
    unreserve(J, reinterpret_cast<void *>(decode(Was)));
}

char *slotOf(uint32_t Index, const Image &I, uint64_t Place) {
  uint64_t Arena = probeLoad(homeOf(Index / 64, Place));
  return Arena ? reinterpret_cast<char *>(decode(Arena)) +
                     (Index % 64 - 16) * slotSize(I)
               : nullptr;
}

//===----------------------------------------------------------------------===//
// Installation
//===----------------------------------------------------------------------===//

enum How { Seed, Walk, Again };

// Whether a record's thread may still run code of the model.
bool alive(uint64_t W) { return stateOf(W) == Claimed || stateOf(W) == Live; }

// Writes image I's template into the thread of record Index: the whole range
// a seed or a walk; again, each word that the thread still holds unfilled. A
// wide image's word is written first, with its slot committed where it is a
// page or more, by the seed or for the walk; a walk then reads the record,
// and returns the slot where the record has begun its exit meanwhile, since
// whichever of the two runs last leaves it returned. False where the slot is
// refused.
bool install(const Image &I, uint64_t Place, uint32_t Index, How H) {
  char *Slot = isWide(I) ? slotOf(Index, I, Place) : nullptr;
  uint64_t *State = &recordAt(Index).Word,
           Word = Slot ? encode(uint64_t(Slot)) : 0;
  if ((isWide(I) && !Slot) ||
      (H == Seed && paged(I) && !commit(Slot, slotSize(I))))
    return false;
  auto *To = reinterpret_cast<uint64_t *>(blockAt(Index) + Place);
  if (!Slot || H == Again || __wincrt_probe_copy(To, &Word, 1)) {
    // The template is read by probe, as its image may be unloading.
    auto *From = reinterpret_cast<const uint64_t *>(I.Tls);
    To = Slot ? reinterpret_cast<uint64_t *>(Slot) : To;
    size_t Words = granules(I) * Granule / 8;
    if (H != Again)
      __wincrt_probe_copy(To, From, Words);
    else
      for (uint64_t W = 0, Value; W != Words; ++W)
        if (__wincrt_probe_copy(&Value, &From[W], 1) && Value != TP_UNFILLED)
          __wincrt_probe_cas(&To[W], TP_UNFILLED, Value);
  }
  if (H != Seed && paged(I) && !alive(probeLoad(State)))
    decommit(Slot, slotSize(I));
  return true;
}

// Each registered image, and its place: an image that its fill has not
// published, or that is unmapped under the read, has none and is passed over,
// since no place is 0 or unfilled.
template <typename F> void eachImage(F Each) {
  eachEntry([&](Entry &E, uint64_t W) {
    Image I;
    if (!(W & RegionEntry) && (W & StateMask) == EntryLive &&
        readImage(reinterpret_cast<char *>(load(&E.Base)), I))
      if (uint64_t Place = placeOf(I); Place && Place != TP_UNFILLED)
        Each(I, Place);
    return false;
  });
}

//===----------------------------------------------------------------------===//
// Releasing what a scan finds unheld
//===----------------------------------------------------------------------===//

// Releases what the set's removed or closed entries kept, once no step
// holding an era older than the stamp's can still act on it: a wide image's
// arenas and its word, a region's address space. One compare-exchange claims
// each. The bound starts at the era read before the holds: a step that read an
// entry live before its stamp holds an era below the stamp's, and one these
// reads miss took its hold after them, so the stamp's era is past the bound.
void scan(Journal &J) {
  uint64_t Oldest = load(&shared().Era);
  auto Older = [&](uint64_t H) { Oldest = H && H < Oldest ? H : Oldest; };
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C)
    Older(load(&chunkAt(C).Hold));
  eachRecord([&](uint32_t Index) { Older(probeLoad(&recordAt(Index).Hold)); });
  eachEntry([&](Entry &E, uint64_t W) {
    if ((W & StateMask) != Removing || eraOf(W) > Oldest ||
        !cas(&E.Word, W, (W & ~StateMask) | Released))
      return false;
    if (W & RegionEntry) {
      unreserve(J, reinterpret_cast<void *>(regionBase(E)));
    } else {
      for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C)
        releaseArena(C, E.Taken[0], J);
      if (Placed P = placedAt(E.Taken[0]); P.In.Taken)
        givePlace(P, 1, J);
    }
    add(&shared().Holders, -1);
    add(&shared().Pending, -1);
    leave(E);
    return false;
  });
}

} // namespace

void wincrt::tls::settle(Journal &J) {
  if (J.Freeing.Base && !J.Freeing.Size)
    unreserve(J, J.Freeing.Base);
  if (void *Base = J.Making.Base)
    if (probeLoad(J.Home) != encode(uint64_t(Base)))
      unreserve(J, Base);
  J = {};
}

//===----------------------------------------------------------------------===//
// Seeds, revivals, parks and ends
//===----------------------------------------------------------------------===//

// A wide image's arena of the seeded block's chunk is made by the chunk's
// first seed, which its claim keeps from parking meanwhile. A seed that took
// no hold, there being no wide or overflow entry, leaves one that appears
// meanwhile to its registration's walk.
bool wincrt::tls::seedImages(uint32_t Index) {
  Record &R = recordAt(Index);
  bool Held = hold(R.Hold), Made = true;
  eachImage([&](Image &I, uint64_t Place) {
    if (Held || (!isWide(I) && Place < 2 * TP_PLANE + TP_BLOCK))
      Made = Made && (!isWide(I) || makeArena(Index / 64, I, Place, R.J)) &&
             install(I, Place, Index, Seed);
  });
  unhold(R.Hold, R.J);
  return Made;
}

bool wincrt::tls::reviveHook(uint32_t C) {
  Chunk &Ch = chunkAt(C);
  if (!hold(Ch.Hold))
    return true;
  bool Done = !eachEntry([&](Entry &E, uint64_t W) {
    for (unsigned P = 0;
         W & RegionEntry && (W & StateMask) == EntryLive && P != 5; ++P)
      if ((positionOf(W, P) == PosOpen || positionOf(W, P) == PosOpening) &&
          !commit(regionRun(E, C, P), TP_PLANE))
        return true;
    return false;
  });
  unhold(Ch.Hold, Ch.J);
  return Done;
}

// A chunk's park releases its arena of every wide image, a removed one's
// among them, whose entry keeps its place since its image is gone, and
// returns its run of every live region.
void wincrt::tls::parkHook(uint32_t C) {
  Chunk &Ch = chunkAt(C);
  if (!hold(Ch.Hold))
    return;
  settle(Ch.J);
  eachEntry([&](Entry &E, uint64_t W) {
    if (W & WideEntry && (W & StateMask) == Removing)
      releaseArena(C, E.Taken[0], Ch.J);
    if (W & RegionEntry && (W & StateMask) == EntryLive)
      decommit(regionRun(E, C, 0), 5 * TP_PLANE);
    return false;
  });
  eachImage([&](Image &I, uint64_t Place) {
    if (isWide(I))
      releaseArena(C, Place, Ch.J);
  });
  unhold(Ch.Hold, Ch.J);
}

// A removed image's slots return with its arena, which a step holding a
// later era than the removal's does not touch.
void wincrt::tls::endWide(Record &Self, uint32_t Index) {
  if (!hold(Self.Hold))
    return;
  eachImage([&](Image &I, uint64_t Place) {
    if (char *Slot = paged(I) ? slotOf(Index, I, Place) : nullptr)
      decommit(Slot, slotSize(I));
  });
  unhold(Self.Hold, Self.J);
}

void wincrt::tls::endHold(Record &Self, Record &Dead) {
  settle(Dead.J);
  unhold(Dead.Hold, Self.J);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

// Commits a wide image's slots of a page or more for chunk C's records in
// Mine, one run of consecutive slots at a time.
static bool commitRuns(uint32_t C, const Image &I, uint64_t Place,
                       uint64_t Mine) {
  for (uint64_t M = paged(I) ? Mine : 0; M;) {
    unsigned S = __builtin_ctzll(M), N = __builtin_ctzll(~(M >> S));
    char *Slot = slotOf(C * 64 + S, I, Place);
    if (Slot && !commit(Slot, N * slotSize(I)))
      return false;
    M &= ~(run(N) << S);
  }
  return true;
}

// Places the image at Base and writes its template into every thread, or
// answers its place again where the set holds it, writing each word a thread
// holds unfilled. Answers 0 for a refusal.
static uint64_t registerImage(char *Base) {
  Image I;
  Header *H = header(true);
  if (!readImage(Base, I) || !H ||
      (I.Place &&
       load(&I.Place[1]) != (isWide(I) ? TP_FORM_WIDE : TP_FORM_DIRECT)))
    return 0;
  Record &Self = recordAt(lookup(currentTid()) & TP_MASK);
  unsigned N = isWide(I) ? 1 : granules(I);
  How Pass = find(Base) ? Again : Walk;
  // A wide image's word: the wide area's, or a granule placed as a direct
  // image's storage is.
  Placed P = Pass == Again ? Placed{placeOf(I), {}}
                           : place(N, isWide(I) ? 1 : (I.Align + Granule - 1) / Granule,
                                   isWide(I), Self.J);
  if (Pass == Walk) {
    Entry *E = P.Place ? admit(uint64_t(Base)) : nullptr;
    if (!E) {
      if (P.In.Taken)
        givePlace(P, N, Self.J);
      return 0;
    }
    if (isWide(I))
      add(&shared().Holders, 1);
    store(&E->Word, isWide(I) ? WideEntry | EntryLive : EntryLive);
  }
  // Each chunk's blocks are written as its protector, so that none of its
  // records is finished and it does not park under the writes, but for those
  // whose threads have begun their exit.
  add(&shared().CreateGen, 1);
  hold(Self.Hold);
  bool Made = true;
  eachChunk([&](uint32_t C, uint64_t) {
    shield(Self, C * 64);
    uint64_t W = load(&chunkAt(C).Word), Mine = 0;
    for (uint64_t T = (W & ChunkState) == ChunkLive ? ~W & FullBits : 0; T;
         T &= T - 1)
      if (alive(probeLoad(&recordAt(C * 64 + __builtin_ctzll(T)).Word)))
        Mine |= T & -T;
    Made = Made &&
           (!Mine || !isWide(I) || makeArena(C, I, P.Place, Self.J)) &&
           commitRuns(C, I, P.Place, Mine);
    for (; Made && Mine; Mine &= Mine - 1)
      Made = install(I, P.Place, C * 64 + __builtin_ctzll(Mine), Pass);
    unprotect(Self);
  });
  unhold(Self.Hold, Self.J);
  return Made ? P.Place : 0;
}

// An unloaded direct image's granules return, closing a plane or a position
// it leaves empty, and its base leaves the set. A wide image is stamped
// removing, and its arenas and its word are released by a scan once no older
// hold stands. Its code is gone, so no thread reads its thread-locals again.
static void unloadImage(char *Base) {
  Entry *E = find(Base);
  Image I;
  Header *H = header(false);
  if (!E || !readImage(Base, I) || !H)
    return;
  Record &Self = recordAt(lookup(currentTid()) & TP_MASK);
  uint64_t Place = placeOf(I), W = load(&E->Word);
  if (!isWide(I)) {
    if (Placed P = placedAt(Place); P.In.Taken)
      givePlace(P, granules(I), Self.J);
    return leave(*E);
  }
  E->Taken[0] = Place;
  if (stamp(*E, W, (W & ~StateMask) | Removing))
    scan(Self.J);
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

// The key: drawn once, with its fixed bits, into a page of a pagefile-backed
// section that cannot change protection, written through a view that is then
// unmapped, and read through a read-only one.
static const uint64_t *drawKey() {
  unsigned Draw[2] = {};
  if (__wrap_rand_s(&Draw[0]) || __wrap_rand_s(&Draw[1]))
    return nullptr;
  uint64_t Key =
      ((uint64_t(Draw[1]) << 32 | Draw[0]) | TP_KEY_SET) & ~TP_KEY_CLEAR;
  HANDLE Section;
  LARGE_INTEGER Size = {{Page}};
  if (NtCreateSection(&Section, SECTION_ALL_ACCESS, nullptr, &Size,
                      PAGE_READWRITE, SEC_COMMIT | SEC_NO_CHANGE, nullptr) < 0)
    return nullptr;
  auto *Write = static_cast<uint64_t *>(
      MapViewOfFile(Section, FILE_MAP_WRITE, 0, 0, Page));
  auto *Read = Write ? static_cast<const uint64_t *>(
                           MapViewOfFile(Section, FILE_MAP_READ, 0, 0, Page))
                     : nullptr;
  if (Write) {
    *Write = Key;
    UnmapViewOfFile(Write);
  }
  CloseHandle(Section);
  return Read && *Read == Key ? Read : nullptr;
}

// The executable is registered at founding with its template as linked, its
// place the link's, and a wide one's key word written; its fill registers it
// again. The loader's notification is found by name, as its documentation
// has it, and has no KCFI type.
__attribute__((no_sanitize("kcfi"))) bool wincrt::tls::foundImages() {
  using Register = NTSTATUS(NTAPI *)(
      ULONG, void(CALLBACK *)(ULONG, const DllNotification *, void *), void *,
      void **);
  auto Notify = reinterpret_cast<Register>(GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
  void *Cookie;
  const uint64_t *Key = drawKey();
  if (!Key || !Notify || Notify(0, notified, nullptr, &Cookie) < 0 ||
      !bracket(&__tp_key, *Key))
    return false;
  store(&shared().Key, Key);
  shared().Era = 1;
  shared().Wide = ~uint64_t(0xFE);
  Image I;
  if (!readImage(executable(), I))
    return true;
  uint64_t Place = executablePlace(I);
  if (Place == TP_PLANE)
    shared().Taken[1] = ~uint64_t(0), shared().Planes |= uint64_t(1) << 1;
  else if (Place == TP_EXE_PLACE)
    shared().Taken[0] |= run(granules(I)) << TP_EXE_PLACE / Granule;
  else if (!I.Place || !bracket(&I.Place[2], *Key))
    return false;
  Entry *E = admit(uint64_t(executable()));
  if (E && isWide(I))
    add(&shared().Holders, 1);
  return E &&
         (store(&E->Word, isWide(I) ? WideEntry | EntryLive : EntryLive), true);
}

extern "C" WINCRT_TLS_API WINCRT_NO_TP uint64_t __tp_register(void *Base) {
  return registerImage(static_cast<char *>(Base));
}
