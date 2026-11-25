//===-- tlsrt.cpp - The TID map and the block area ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The TID map is one u32 per client id, at the map plus the id, in two slots:
// A, which every derivation reads, and B, which holds the entries published
// while a page of A is being returned. A page of either returns when it holds
// no entry; a publisher that meets A's return writes B, and the two never
// return together, so no publication lands on a page a decided decommit is
// about to take.
//
// A chunk is claimed slot by slot: a record by one compare-exchange that
// writes its client id, then its bit by one compare-exchange that requires
// the chunk live. The release that frees a chunk's last bit parks it, sealing
// every record a claimer left without its bit and returning the chunk's
// pages. A revival or a park names its actor in the chunk's entry, so that
// the rotating check finishes one whose actor is gone.
//
//===----------------------------------------------------------------------===//

#include "tlsrt.h"

#pragma comment(lib, "ntdll")

#include <errno.h>

using namespace wincrt::tls;

// The exports, alone on their own read-only section.
#pragma section(".wtls", read)
extern "C" {
__declspec(allocate(".wtls")) const volatile uintptr_t __tp_map = 0;
__declspec(allocate(".wtls")) const volatile uintptr_t __tp_base = 0;
__declspec(allocate(".wtls")) const volatile uintptr_t __tp_key = 0;
__declspec(align(64)) const uint64_t __tp_none[TP_H / 8] = {
    reinterpret_cast<uint64_t>(&__tp_none), 0,
    reinterpret_cast<uint64_t>(&__tp_none)};
}

bool wincrt::tls::commit(void *Base, uintptr_t Size) {
  SIZE_T N = Size;
  return NtAllocateVirtualMemoryEx(GetCurrentProcess(), &Base, &N, MEM_COMMIT,
                                   PAGE_READWRITE, nullptr, 0) >= 0;
}

void wincrt::tls::decommit(void *Base, uintptr_t Size) {
  SIZE_T N = Size;
  NtFreeVirtualMemory(GetCurrentProcess(), &Base, &N, MEM_DECOMMIT);
}

//===----------------------------------------------------------------------===//
// The map
//===----------------------------------------------------------------------===//

namespace {

// A page's control: a bit per id with an entry, and per id whose entry is in
// B; the page's word, whether each slot's page is committed (CA, CB) or being
// returned (RA, RB), and the returner's client id. A returner flags its slot
// in the word, then reads the bits; a publisher sets its bit, then reads the
// word, each by a read-modify-write: one sees the other.
struct Control {
  uint64_t Ids[16];
  uint64_t InB[16];
  uint64_t W;
  uint64_t Pad[31];
};
static_assert(sizeof(Control) == 512);
enum : uint64_t { CA = 1, CB = 2, RA = 4, RB = 8 };

uint32_t *slot(uint32_t Tid, bool B) {
  return reinterpret_cast<uint32_t *>(meta() + B * MapSpan + Tid);
}
void *pageOf(uint32_t P, bool B) {
  return meta() + B * MapSpan + uintptr_t(P) * Page;
}
Control &control(uint32_t P) {
  return reinterpret_cast<Control *>(meta() + 2 * MapSpan)[P];
}

bool committed(uint32_t P) {
  return load(&shared().ControlCommitted[P / 8 / 64]) >> (P / 8 % 64) & 1;
}

// The control of a page, committed. Eight share a page of their own, which
// is committed once and kept.
Control *controlFor(uint32_t P) {
  if (!committed(P)) {
    if (!commit(&control(P & ~7u), Page))
      return nullptr;
    setBits(&shared().ControlCommitted[P / 8 / 64], uint64_t(1) << P / 8 % 64);
  }
  return &control(P);
}

// Whether an id of the page has an entry in B, or in either slot, which
// keeps A. A plain load decides only to look further; a nonzero one, which
// could be stale, is confirmed by a read-modify-write.
bool holds(Control &C, bool B) {
  for (uint64_t &W : B ? C.InB : C.Ids)
    if (load(&W) && peek(&W))
      return true;
  return false;
}

void evaluate(Record &Self, uint32_t P);

// Returns a slot's page where the slot holds no entry, and A again where a
// publisher filled B meanwhile, so that its derivation reads zero rather than
// faulting; then ends the return and evaluates the page again.
void returnSlot(Record &Self, Control &C, uint32_t P, bool B) {
  if (!holds(C, B)) {
    decommit(pageOf(P, B), Page);
    if (clearBits(&C.W, B ? CB : CA) & CB && !B && commit(pageOf(P, B), Page))
      setBits(&C.W, CA);
  }
  clearBits(&C.W, (B ? RB : RA) | ~uint64_t(0) << 32);
  store(&Self.Return, 0);
  evaluate(Self, P);
}

// Returns a slot of the page that holds no entry, B first, each by one
// compare-exchange that names the returner, journalled in its record.
void evaluate(Record &Self, uint32_t P) {
  Control &C = control(P);
  for (int B = 1; B >= 0; --B) {
    uint64_t W = load(&C.W);
    if (W & (RA | RB))
      return;
    if (!(W & (B ? CB : CA)) || holds(C, B))
      continue;
    store(&Self.Return, P + 1);
    if (cas(&C.W, W, W | (B ? RB : RA) | uint64_t(currentTid()) << 32))
      return returnSlot(Self, C, P, B);
    return store(&Self.Return, 0);
  }
}

// Publishes Tid's entry, its first and only write.
bool publish(uint32_t Tid, uint32_t Entry) {
  uint32_t P = Tid / Page, Id = Tid / 4 % 1024;
  if (P < Prefix)
    return store(slot(Tid, false), Entry), true;
  Control *C = P < MapPages ? controlFor(P) : nullptr;
  if (!C)
    return false;
  uint64_t *Occupied = &C->Ids[Id / 64], *InB = &C->InB[Id / 64];
  uint64_t Bit = uint64_t(1) << Id % 64;
  setBits(Occupied, Bit);
  // A's return is in flight: B, unless B's has begun since, which A's ended
  // before and Id's bit now blocks. A is committed beside B, so that the
  // derivation reads zero there.
  uint64_t W = peek(&C->W);
  bool B = W & RA;
  if (B) {
    if (!commit(pageOf(P, true), Page))
      return clearBits(Occupied, Bit), false;
    setBits(&C->W, CB);
    setBits(InB, Bit);
    if ((W = peek(&C->W)) & RB)
      B = (clearBits(InB, Bit), false);
  }
  if (!(W & CA)) {
    if (commit(pageOf(P, false), Page))
      setBits(&C->W, CA);
    else if (!B)
      return clearBits(Occupied, Bit), false;
  }
  store(slot(Tid, B), Entry);
  return true;
}

// The entry's owner writes it: the thread, or its finisher while its pin
// keeps the id. Its occupancy keeps A's page, and its B bit B's.
bool inB(uint32_t Tid) {
  uint32_t P = Tid / Page, Id = Tid / 4 % 1024;
  return P >= Prefix && load(&control(P).InB[Id / 64]) >> Id % 64 & 1;
}

void clear(Record &Self, uint32_t Tid, uint32_t Entry) {
  uint32_t P = Tid / Page, Id = Tid / 4 % 1024, Tomb = TP_TOMBSTONE;
  if (P >= Prefix && !committed(P))
    return;
  // The id's occupancy, cleared after its entry, keeps the entry's page: a
  // rerun that finds it clear finds the entry cleared, and its page perhaps
  // returned.
  if (uint32_t *E = slot(Tid, inB(Tid));
      P < Prefix || load(&control(P).Ids[Id / 64]) >> Id % 64 & 1)
    cas(E, Entry, 0) || cas(E, Tomb, 0);
  if (P < Prefix)
    return;
  Control &C = control(P);
  clearBits(&C.InB[Id / 64], uint64_t(1) << Id % 64);
  clearBits(&C.Ids[Id / 64], uint64_t(1) << Id % 64);
  evaluate(Self, P);
}

// Resumes the return that a dead record's journal names, where its word
// still names the dead thread.
void resumeReturn(Record &Self, Record &Dead) {
  uint64_t P = load(&Dead.Return);
  if (!P--)
    return;
  Control &C = control(P);
  uint64_t W = load(&C.W);
  if (W >> 32 == tidOf(Dead.Word) && W & (RA | RB) &&
      cas(&C.W, W, (W & 0xFFFFFFFF) | uint64_t(currentTid()) << 32)) {
    store(&Self.Return, P + 1);
    returnSlot(Self, C, P, !(W & RA));
  }
  store(&Dead.Return, 0);
}

} // namespace

uint32_t wincrt::tls::lookup(uint32_t Tid) {
  uint32_t P = Tid / Page;
  if (P < Prefix)
    return load(slot(Tid, false));
  if (P >= MapPages || !committed(P))
    return 0;
  uint64_t W = load(&control(P).W);
  uint32_t E = W & CA ? __wincrt_probe32(slot(Tid, false)) : 0;
  return E || !(W & CB) ? E : __wincrt_probe32(slot(Tid, true));
}

//===----------------------------------------------------------------------===//
// Chunks and records
//===----------------------------------------------------------------------===//

// Whether a thread's handle is signalled: the thread is past its last
// instruction.
static bool signalled(HANDLE Thread) {
  LARGE_INTEGER Zero = {};
  return NtWaitForSingleObject(Thread, FALSE, &Zero) == STATUS_WAIT_0;
}

// Whether the thread with client id Tid, whose handle no record holds, is
// gone: its id names no thread, or a signalled one.
static bool gone(uint32_t Tid) {
  CLIENT_ID Id = {nullptr, reinterpret_cast<HANDLE>(uintptr_t(Tid))};
  OBJECT_ATTRIBUTES Attributes = {sizeof(Attributes)};
  HANDLE Thread;
  NTSTATUS Status = NtOpenThread(&Thread, SYNCHRONIZE, &Attributes, &Id);
  if (Status < 0)
    return Status == STATUS_INVALID_CID;
  bool Gone = signalled(Thread);
  NtClose(Thread);
  return Gone;
}

// Returns a chunk whose revival is undone or whose park completes.
static void empty(uint32_t C) {
  parkHook(C);
  decommit(chunkBase(C), ChunkSize);
  store(&chunkAt(C).Word, ChunkEmpty);
}

// Commits a chunk and every open plane, or answers false having committed
// nothing that stays.
static bool revive(uint32_t C) {
  Chunk &Ch = chunkAt(C);
  uint64_t Actor = 0, W = load(&Ch.Word);
  if (((W & ChunkState) != ChunkEmpty && (W & ChunkState) != ChunkParked) ||
      !cas(&Ch.Actor, Actor, currentTid()))
    return false;
  bool Revived = cas(&Ch.Word, W, ChunkReviving);
  if (Revived) {
    // Read after the state's change: an opening plane either reads the
    // chunk reviving or has published its bit here.
    uint64_t Planes = peek(&shared().Planes);
    Revived = commit(chunkBase(C), 64 * TP_BLOCK);
    for (unsigned P = 1; Revived && P <= TP_PLANES; ++P)
      Revived = !(Planes >> P & 1) || commit(planeRun(C, P), TP_PLANE);
    Revived = Revived && reviveHook(C);
    if (Revived)
      store(&Ch.Word, ChunkLive | FullBits);
    else
      empty(C);
  }
  store(&Ch.Actor, 0);
  return Revived;
}

// Takes a free slot of a live chunk for Tid: its record, then its bit.
static uint32_t takeIn(uint32_t C, uint32_t Tid) {
  Chunk &Ch = chunkAt(C);
  for (uint64_t Bits = load(&Ch.Word);
       (Bits & ChunkState) == ChunkLive && (Bits &= FullBits);
       Bits &= Bits - 1) {
    unsigned Slot = __builtin_ctzll(Bits);
    uint64_t *R = &recordAt(C * 64 + Slot).Word, Old = load(R),
             Mine = recordWord(Claimed, Tid, Old);
    if (stateOf(Old) != Free || !cas(R, Old, Mine))
      continue;
    for (uint64_t W = load(&Ch.Word);
         (W & ChunkState) == ChunkLive && W >> Slot & 1;)
      if (cas(&Ch.Word, W, W & ~(uint64_t(1) << Slot)))
        return store(&shared().Hint, C), C * 64 + Slot;
    // Its chunk is parking, which seals the record, or another took the bit.
    cas(R, Mine, recordWord(Free, 0, Mine));
  }
  return 0;
}

// Takes a slot of a live chunk, else revives a parked or returned one, else
// publishes a chunk never used, whose entry's page is committed first. Each
// pass that takes nothing follows another thread's take or publication, or
// this one's, and the chunks are bounded.
static uint32_t claim(uint32_t Tid) {
  for (;;) {
    uint32_t Top = load(&shared().Top), Hint = load(&shared().Hint);
    for (uint32_t I = 0; I != Top; ++I)
      if (uint32_t Index = takeIn((Hint + I) % Top + 1, Tid))
        return Index;
    for (uint32_t I = 1; I <= Top; ++I)
      if (uint32_t Index = revive(I) ? takeIn(I, Tid) : 0)
        return Index;
    if (Top + 1 >= Chunks || !commit(&chunkAt(Top + 1), sizeof(Chunk)))
      return 0;
    if (cas(&shared().Top, Top, Top + 1))
      revive(Top + 1);
  }
}

// Parks a chunk whose last bit the caller freed: seals every record left
// without its bit, then returns the chunk's pages, or keeps them while a
// plane opens or closes, whose changer returns it.
void wincrt::tls::park(uint32_t C) {
  Chunk &Ch = chunkAt(C);
  uint64_t Actor = 0, W = ChunkLive | FullBits;
  if (!cas(&Ch.Actor, Actor, currentTid()))
    return;
  if (cas(&Ch.Word, W, ChunkParking) || (W & ChunkState) >= ChunkParking) {
    for (unsigned Slot = 16; Slot != 63; ++Slot) {
      uint64_t *R = &recordAt(C * 64 + Slot).Word, Old = load(R);
      if (stateOf(Old) != Free)
        cas(R, Old, recordWord(Free, 0, Old));
    }
    if (peek(&shared().Planes) & Change)
      store(&Ch.Word, ChunkParked);
    else
      empty(C);
  }
  store(&Ch.Actor, 0);
}

static void returnBit(uint32_t Index) {
  uint64_t Bit = uint64_t(1) << Index % 64;
  if (add(&chunkAt(Index / 64).Word, Bit) + Bit == (ChunkLive | FullBits))
    park(Index / 64);
}

static void release(uint32_t Index) {
  Record &R = recordAt(Index);
  uint64_t C = load(&chunkAt(Index / 64).Word);
  // The word, then the bit: a record whose claimer died before its bit holds
  // none, and no claim reads a record whose bit is clear, so a free record
  // with its bit clear is a return a rerun completes.
  bool Held = (C & ChunkState) == ChunkLive && !(C >> Index % 64 & 1);
  for (uint64_t W = load(&R.Word);
       stateOf(W) != Free && !cas(&R.Word, W, recordWord(Free, 0, W));)
    ;
  if (Held)
    returnBit(Index);
}

// Whether a record's thread holds it: neither free nor being finished.
static bool inUse(uint64_t W) {
  return stateOf(W) != Free && stateOf(W) != Finishing;
}

// Moves the caller's own record to State.
static bool advance(Record &R, uint64_t State) {
  for (uint64_t W = load(&R.Word); inUse(W);)
    if (cas(&R.Word, W, recordWord(State, tidOf(W), W)))
      return true;
  return false;
}

// Finishes a chunk's revival or park whose actor is gone: the revival is
// undone, since no slot of a reviving chunk is claimed, and the park run.
static void finishChunk(uint32_t C) {
  Chunk &Ch = chunkAt(C);
  uint64_t Actor = load(&Ch.Actor);
  if (!Actor || !gone(uint32_t(Actor)) || !cas(&Ch.Actor, Actor, currentTid()))
    return;
  uint64_t W = load(&Ch.Word);
  settle(Ch.J);
  if ((W & ChunkState) == ChunkReviving)
    empty(C);
  store(&Ch.Actor, 0);
  if ((W & ChunkState) == ChunkParking)
    park(C);
}

//===----------------------------------------------------------------------===//
// Adoption
//===----------------------------------------------------------------------===//

static void check(Record &Self);

// Zeroes the block and its slot of every open plane, then copies every
// registered image's template, again while a registration's generation moves:
// either the registration's walk reaches the block, or the seed reads the
// raised generation. A plane read open may close under the seed, which zeroes
// by probe.
static bool seed(uint32_t Index) {
  static const uint64_t Zeros[TP_BLOCK / 8] = {};
  char *Tp = blockAt(Index);
  for (uint64_t Gen = load(&shared().CreateGen);;
       Gen = load(&shared().CreateGen)) {
    uint64_t Planes = load(&shared().Planes);
    __builtin_memset(Tp, 0, TP_BLOCK);
    for (unsigned P = 1; P <= TP_PLANES; ++P)
      if (Planes >> P & 1)
        __wincrt_probe_copy(Tp + P * TP_PLANE, Zeros, TP_BLOCK / 8);
    auto &H = *reinterpret_cast<Header *>(Tp);
    H.TcbSelf = H.Self = reinterpret_cast<uintptr_t>(Tp);
    if (!seedImages(Index))
      return false;
    if (load(&shared().CreateGen) == Gen)
      return true;
  }
}

// Each resource is in the record before the next is taken: the client id by
// the claim, then the pin, the system call's output, then the block, so that
// a kill anywhere leaves the record what the rotating check finishes. The
// TID map's entry, published last, is the adoption's linearization point. A
// refused adoption leaves its record to the rotating check, which finishes
// it once the thread is gone.
intptr_t wincrt::tls::adopt(uint32_t Tid, HANDLE Thread) {
  uint32_t Entry = lookup(Tid);
  if (Entry & TP_SLOT_TEST)
    return reinterpret_cast<intptr_t>(blockAt(Entry & TP_MASK));
  if (Entry == TP_TOMBSTONE)
    return -ESRCH;
  uint32_t Index = claim(Tid);
  if (!Index)
    return -EAGAIN;
  Record &R = recordAt(Index);
  if (NtDuplicateObject(GetCurrentProcess(), Thread, GetCurrentProcess(),
                        &R.Pin, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                        OBJ_PROTECT_CLOSE, 0) < 0 ||
      !seed(Index) || !advance(R, Live) || !publish(Tid, Index))
    return advance(R, Releasing), -EAGAIN;
  if (Tid == currentTid())
    check(R);
  return reinterpret_cast<intptr_t>(blockAt(Index));
}

Header *wincrt::tls::header(bool Adopt) {
  uint32_t Entry = lookup(currentTid());
  intptr_t Tp = Adopt ? adopt(currentTid(), GetCurrentThread())
                : Entry & TP_SLOT_TEST
                    ? reinterpret_cast<intptr_t>(blockAt(Entry & TP_MASK))
                    : -1;
  return Tp < 0 ? nullptr : reinterpret_cast<Header *>(Tp);
}

//===----------------------------------------------------------------------===//
// Thread end
//===----------------------------------------------------------------------===//

// A finisher's protection while it finishes: the record, the generation of
// the word its election wrote, and whether only the free step runs.
enum : uint64_t { Finisher = uint64_t(1) << 63, FreeOnly = uint64_t(1) << 62 };
constexpr unsigned GenShift = 22;

// The pin is protected from close from its duplicate, so an untagged value is
// the record's own. Its closer writes the thread's creation time, tags the
// value, unprotects and closes the handle, and clears the field last; a rerun
// closes a tagged value only where it still names the thread of the record's
// client id and creation time, since a closed value may name another handle.
static void closePin(Record &R, uint32_t Tid) {
  auto Pin = reinterpret_cast<uintptr_t>(load(&R.Pin));
  auto *Thread = reinterpret_cast<HANDLE>(Pin & ~uintptr_t(3));
  int64_t Times[4]; // creation, exit, kernel and user
  bool Timed = Pin && NtQueryInformationThread(Thread, THREADINFOCLASS(1),
                                               Times, sizeof(Times),
                                               nullptr) >= 0;
  if (!Pin)
    return;
  if (!(Pin & 1)) {
    store(&R.PinTime, Timed ? uint64_t(Times[0]) : 0);
    store(&R.Pin, reinterpret_cast<HANDLE>(Pin | 1));
  } else if (!Timed || uint64_t(Times[0]) != load(&R.PinTime) ||
             GetThreadId(Thread) != Tid) {
    return store(&R.Pin, HANDLE(nullptr));
  }
  struct {
    BOOLEAN Inherit, ProtectFromClose;
  } Unprotected = {};
  NtSetInformationObject(Thread, 4 /*ObjectHandleFlagInformation*/,
                         &Unprotected, sizeof(Unprotected));
  NtClose(Thread);
  store(&R.Pin, HANDLE(nullptr));
}

static void finish(Record &Self, uint32_t Index, bool Seeded);

// The finish a dead finisher left: run again while the record holds the word
// its election wrote, or its bit returned where the record is free from that
// word with its bit still clear.
static void rerun(Record &Self, Record &Dead) {
  uint64_t P = load(&Dead.Protecting);
  if (!(P & Finisher))
    return;
  uint32_t Index = uint32_t(P & ((uint64_t(1) << GenShift) - 1)) - 1;
  uint32_t Gen = uint32_t(P >> GenShift);
  uint64_t W = probeLoad(&recordAt(Index).Word);
  uint64_t C = load(&chunkAt(Index / 64).Word);
  if (stateOf(W) == Finishing && uint32_t(W) == Gen)
    finish(Self, Index, !(P & FreeOnly));
  else if (!(P & FreeOnly) && stateOf(W) == Free && !tidOf(W) &&
           uint32_t(W) == Gen + 1 && (C & ChunkState) == ChunkLive &&
           !(C >> Index % 64 & 1))
    returnBit(Index);
}

// What a thread leaves, which its release at its last hook begins and its
// finisher ends: its wide slots of a page or more decommitted and its
// destructor list dropped; then its TID-map entry tombstoned, or cleared, its
// hold cleared and the scan run, its pin closed, and the record and its block
// freed, of which a record that seeded no block has only the last. Every step
// is idempotent, and keeps where it is when run again: the map's entry moves
// by compare-exchange from the value it replaces, a decommit changes nothing
// twice, and the pin stays in the record until it is closed. A dead
// protector's protection ends with its hold, and a dead finisher's with the
// finish it left.
static void leave(Record &Self, uint32_t Index) {
  endWide(Self, Index);
  dropThread(reinterpret_cast<Header *>(blockAt(Index))->Thread);
}

static void finish(Record &Self, uint32_t Index, bool Seeded) {
  Record &R = recordAt(Index);
  uint32_t Tid = tidOf(load(&R.Word));
  if (Seeded) {
    leave(Self, Index);
    resumeReturn(Self, R);
    clear(Self, Tid, Index);
    endHold(Self, R);
    rerun(Self, R);
    unprotect(R);
    closePin(R, Tid);
  }
  release(Index);
}

void wincrt::tls::releaseSelf() {
  uint32_t Entry = lookup(currentTid()), Index = Entry & TP_MASK;
  Record &R = recordAt(Index);
  if (!(Entry & TP_SLOT_TEST) || !advance(R, Releasing))
    return;
  uint32_t Tid = tidOf(load(&R.Word));
  leave(R, Index);
  cas(slot(Tid, inB(Tid)), Index, TP_TOMBSTONE);
  check(R);
}

// Whether a thread other than Self's protects record Index, or its chunk.
static bool protectedBy(Record &Self, uint32_t Index) {
  bool Found = false;
  eachRecord([&](uint32_t I) {
    uint64_t P = probeLoad(&recordAt(I).Protecting);
    Found |= &recordAt(I) != &Self && (P == Index + 1 || P == (Index & ~63u) + 1);
  });
  return Found;
}

// A shared cursor runs over every slot of the published chunks. Each step
// takes the next records that a thread holds, up to two, and waits on each
// one's pin with a zero timeout as its protector, which keeps the pin open: a
// signalled pin is a thread past its last instruction, whose record's one
// finisher a compare-exchange to finishing picks, and which gives the record
// back where another protects it. A protector publishes the record in its
// own, then reads the record's state; an elector moves the state to
// finishing, then reads every protector's: of the two read-modify-writes and
// reads, one sees the other. A record killed before its pin is finished
// where its client id names no thread; a chunk's revival or park, at the
// chunk's first slot, where its actor is gone.
static void check(Record &Self) {
  uint32_t Top = load(&shared().Top);
  for (unsigned Held = 0, Seen = 0; Top && Held != 2 && Seen != 64; ++Seen) {
    uint32_t Index =
        uint32_t(add(&shared().Cursor, 1) % (uint64_t(Top) * 64)) + 64;
    uint32_t Slot = Index % 64;
    uint64_t C = load(&chunkAt(Index / 64).Word);
    if (!Slot)
      finishChunk(Index / 64);
    Record &R = recordAt(Index);
    if (Slot < 16 || Slot == 63 || (C & ChunkState) != ChunkLive ||
        !inUse(probeLoad(&R.Word)) || (++Held, &R == &Self))
      continue;
    shield(Self, Index);
    uint64_t W = probeLoad(&R.Word);
    auto Pin = reinterpret_cast<HANDLE>(
        probeLoad(reinterpret_cast<uint64_t *>(&R.Pin)));
    bool Dead = inUse(W) && (Pin ? signalled(Pin) : gone(tidOf(W)));
    W = probeLoad(&R.Word);
    uint64_t Elected = recordWord(Finishing, tidOf(W), W);
    bool Elect = Dead && inUse(W) && __wincrt_probe_cas(&R.Word, W, Elected) == W;
    if (Elect && protectedBy(Self, Index)) {
      __wincrt_probe_cas(&R.Word, Elected,
                         recordWord(stateOf(W), tidOf(W), Elected));
      Elect = false;
    }
    // A claimer killed before its bit seeded no block.
    bool Bitless = C >> Slot & 1;
    if (Elect) {
      store(&Self.Protecting, Finisher | (Bitless ? FreeOnly : 0) |
                                  uint64_t(uint32_t(Elected)) << GenShift |
                                  (Index + 1));
      finish(Self, Index, !Bitless);
    }
    unprotect(Self);
  }
}

// The cold calls' one body, which the assembly entries call having saved
// every register but the answer's: How is 0 for __tp_slow, 1 for __tp_try and
// 2 for __tp_probe, which never adopts.
extern "C" WINCRT_NO_TP uintptr_t __wincrt_tp_cold(const uintptr_t *Map,
                                                   unsigned How) {
  uint32_t Entry = lookup(currentTid());
  if (Entry & TP_SLOT_TEST)
    return reinterpret_cast<uintptr_t>(blockAt(Entry & TP_MASK));
  if (How == 2)
    return reinterpret_cast<uintptr_t>(__tp_none);
  // A released thread's: the closed thread pointer, which the pin keeps its
  // own while the tombstone stands, and every access through which faults.
  if (Entry == TP_TOMBSTONE)
    return How ? uintptr_t(-ESRCH)
               : reinterpret_cast<uintptr_t>(blockAt(TP_CLOSED_SLOT));
  // Only the executable's code runs before its own fill: a DLL's entry that
  // finds its map unwritten fails closed.
  char *Exe = executable();
  intptr_t Tp = !*Map && uintptr_t(reinterpret_cast<const char *>(Map) - Exe) >=
                             wincrt::imageSize(reinterpret_cast<HMODULE>(Exe))
                    ? -ENOEXEC
                    : adopt(currentTid(), GetCurrentThread());
  if (Tp < 0 && !How)
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  return Tp;
}

//===----------------------------------------------------------------------===//
// Founding
//===----------------------------------------------------------------------===//

extern "C" const char __wincrt_probe_begin[], __wincrt_probe_end[],
    __wincrt_probe_landing[], __wincrt_probe_movs[];

// A ULEB128, or an SLEB128 where Signed, and a 4-byte field.
static uint64_t leb(const uint8_t *&P, bool Signed = false) {
  uint64_t Value = 0;
  unsigned Shift = 0;
  uint8_t Byte;
  do {
    Byte = *P++;
    Value |= uint64_t(Byte & 0x7F) << Shift;
    Shift += 7;
  } while (Byte & 0x80);
  return Signed && Shift < 64 && Byte & 0x40 ? Value | ~uint64_t(0) << Shift
                                             : Value;
}
static int32_t field(const uint8_t *P) {
  int32_t Value;
  __builtin_memcpy(&Value, P, 4);
  return Value;
}

// The landing of the compiler's fault probe at Pc: the call site of its
// function's exception table that spans it, whose one catch clause names the
// function itself through its cell, as no invoke's does. The table follows
// the handler's address in the function's unwind information, behind the
// scope table of Windows Itanium's personalities.
static uintptr_t landingOf(uintptr_t Pc) {
  DWORD64 Image;
  const RUNTIME_FUNCTION *Function =
      RtlLookupFunctionEntry(Pc, &Image, nullptr);
  if (!Function)
    return 0;
  auto *Bytes = reinterpret_cast<const uint8_t *>(Image);
  uintptr_t Base = Image + Function->BeginAddress;
#if defined(__x86_64__)
  const uint8_t *Info = Bytes + Function->UnwindData;
  if (!(Info[0] >> 3 & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)))
    return 0;
  const uint8_t *P = Info + 4 + (Info[2] + 1) / 2 * 4 + 4;
#else
  if (Function->UnwindData & 3)
    return 0;
  const uint8_t *X = Bytes + Function->UnwindData;
  uint32_t Header = uint32_t(field(X)), Epilogs = Header >> 22 & 31,
           Words = Header >> 27, Size = 4;
  if (!Epilogs && !Words)
    Epilogs = uint32_t(field(X + 4)) & 0xFFFF,
    Words = uint32_t(field(X + 4)) >> 16 & 0xFF, Size = 8;
  if (!(Header >> 20 & 1))
    return 0;
  const uint8_t *P =
      X + Size + (Header >> 21 & 1 ? 0 : Epilogs * 4) + Words * 4 + 4;
#endif
  if (*P++ != 1)
    return 0;
  P += leb(P);
  uintptr_t LPStart = Base;
  if (uint8_t Encoding = *P++; Encoding == 0x1B)
    LPStart = uintptr_t(P) + field(P), P += 4;
  else if (Encoding != 0xFF)
    return 0;
  if (*P++ != 0x9B)
    return 0;
  uint64_t TTBase = leb(P);
  const uint8_t *Types = P + TTBase;
  if (*P++ != 0x01)
    return 0;
  uint64_t Length = leb(P);
  const uint8_t *Actions = P + Length;
  while (P < Actions) {
    uint64_t Start = leb(P), Span = leb(P), Landing = leb(P), Action = leb(P);
    if (Pc - Base - Start >= Span)
      continue;
    const uint8_t *A = Actions + Action - 1;
    int64_t Filter = Landing && Action ? int64_t(leb(A, true)) : 0;
    if (Filter <= 0)
      return 0;
    const uint8_t *Entry = Types - 4 * Filter;
    uintptr_t Named =
        *reinterpret_cast<const uintptr_t *>(Entry + field(Entry));
    DWORD64 Its;
    const RUNTIME_FUNCTION *Own = RtlLookupFunctionEntry(Named, &Its, nullptr);
    return Named == Base ||
                   (Own && Its == Image && Its + Own->BeginAddress == Named)
               ? LPStart + Landing
               : 0;
  }
  return 0;
}

// Recovers at its landing a fault of the runtime's own probes, or of a
// compiler's probe reading the TID map, where it reads zero.
static LONG CALLBACK recoverProbe(EXCEPTION_POINTERS *Exception) {
  const EXCEPTION_RECORD &Record = *Exception->ExceptionRecord;
  if (Record.ExceptionCode != STATUS_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
#if defined(__x86_64__)
  CONTEXT &Context = *Exception->ContextRecord;
  DWORD64 &Pc = Context.Rip;
  // The copy's rep movsq keeps the caller's rdi and rsi in r9 and r10.
  if (Pc == reinterpret_cast<uintptr_t>(__wincrt_probe_movs)) {
    Context.Rdi = Context.R9;
    Context.Rsi = Context.R10;
  }
#else
  DWORD64 &Pc = Exception->ContextRecord->Pc;
#endif
  uintptr_t Landing =
      Pc - reinterpret_cast<uintptr_t>(__wincrt_probe_begin) <
              uintptr_t(__wincrt_probe_end - __wincrt_probe_begin)
          ? reinterpret_cast<uintptr_t>(__wincrt_probe_landing)
      : !Record.ExceptionInformation[0] &&
              Record.ExceptionInformation[1] - uintptr_t(meta()) < MapSpan
          ? landingOf(Pc)
          : 0;
  if (!Landing)
    return EXCEPTION_CONTINUE_SEARCH;
  Pc = Landing;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Writes Value into a word of read-only image memory under the fill's
// bracket: opened, stored, sealed read-only and read back.
bool wincrt::tls::bracket(volatile const uint64_t *Word, uint64_t Value) {
  auto *To = const_cast<uint64_t *>(Word);
  DWORD Old;
  if (!VirtualProtect(To, sizeof(*To), PAGE_READWRITE, &Old))
    return false;
  // The word may be a constant to the compiler, which the empty asm hides it
  // from.
  __asm__("" : "+r"(To) : : "memory");
  *To = Value;
  return VirtualProtect(To, sizeof(*To), PAGE_READONLY, &Old) && *Word == Value;
}

// Writes the exports, read-only for the process's life.
static bool seal(uintptr_t Map, uintptr_t Base) {
  return bracket(&__tp_map, Map) && bracket(&__tp_base, Base);
}

static void *reserve(uintptr_t Size) {
  void *Base = nullptr;
  SIZE_T N = Size;
  return NtAllocateVirtualMemoryEx(GetCurrentProcess(), &Base, &N, MEM_RESERVE,
                                   PAGE_READWRITE, nullptr, 0) >= 0
             ? Base
             : nullptr;
}

// The first copy of the runtime to attach creates a named section that holds
// its image base, under a name of the process's id and creation time; a
// later copy shares the state of the image it names, which must be one of
// this process's that exports the runtime's names. Answers 1 for the first
// copy, 0 for a later one, which has taken the first one's words, and -1
// where neither holds.
static int rendezvous() {
  FILETIME Created, Unused;
  GetProcessTimes(GetCurrentProcess(), &Created, &Unused, &Unused, &Unused);
  wchar_t Name[] = L"Local\\wincrt-tls-00000000-0000000000000000";
  auto Hex = [&](unsigned At, uint64_t Value, unsigned Digits) {
    for (unsigned I = Digits; I--; Value >>= 4)
      Name[At + I] = L"0123456789abcdef"[Value & 15];
  };
  Hex(17, GetCurrentProcessId(), 8);
  Hex(26, uint64_t(Created.dwHighDateTime) << 32 | Created.dwLowDateTime, 16);
  HANDLE Section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0, Page, Name);
  bool Existed = GetLastError() == ERROR_ALREADY_EXISTS;
  auto *View = Section ? static_cast<HMODULE *>(MapViewOfFile(
                             Section, FILE_MAP_ALL_ACCESS, 0, 0, Page))
                       : nullptr;
  if (!View)
    return -1;
  if (!Existed) {
    *View = reinterpret_cast<HMODULE>(&__ImageBase);
    // The section and its view stay for the process's life.
    return 1;
  }
  HMODULE Other = *View;
  MEMORY_BASIC_INFORMATION Info;
  bool Trusted = VirtualQuery(Other, &Info, sizeof(Info)) &&
                 Info.Type == MEM_IMAGE && Info.AllocationBase == Other;
#define TP_EXPORT(Id, Name, Sealed)                                        \
  Trusted = Trusted && GetProcAddress(Other, #Name);
#include "tls/ThreadPointer.inc"
  UnmapViewOfFile(View);
  CloseHandle(Section);
  if (!Trusted)
    return -1;
  auto Word = [&](const char *Name) {
    return *reinterpret_cast<const uintptr_t *>(GetProcAddress(Other, Name));
  };
  return seal(Word("__tp_map"), Word("__tp_base")) &&
                 bracket(&__tp_key, Word("__tp_key"))
             ? 0
             : -1;
}

bool wincrt::tls::found() {
  int First = rendezvous();
  if (First < 0)
    return false;
  if (!First) {
    State &St = shared();
    return St.Version == StateVersion && St.Size == sizeof(State) &&
           AddVectoredExceptionHandler(1, recoverProbe) && (findTail(), true);
  }
  char *Meta = static_cast<char *>(reserve(StateAt + Page));
  char *Area = static_cast<char *>(reserve(uintptr_t(Chunks) * ChunkSize));
  if (!Meta || !Area || !commit(Meta, Prefix * Page) ||
      !commit(Meta + StateAt, Page) || !seal(uintptr_t(Meta), uintptr_t(Area)))
    return false;
  shared() = {StateVersion, sizeof(State)};
  shared().Cookie = __security_cookie | 1;
  shared().Taken[0] = run(TP_EXE_PLACE / Granule);
  return AddVectoredExceptionHandler(1, recoverProbe) && foundImages() &&
         (findTail(), true);
}

#ifndef COMPILER_RT_SHARED_LIB
// Under -static the executable holds the runtime, founded by its first TLS
// callback, before its entry point and before the fill's callback, which
// .CRT$XLAF holds.
static WINCRT_NO_TP void NTAPI foundStatic(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_ATTACH && !found())
    wincrt::fatal("the thread-local runtime cannot be founded");
}
#pragma section(".CRT$XLAA", read)
extern "C" __declspec(allocate(".CRT$XLAA"))
const PIMAGE_TLS_CALLBACK __wincrt_tls_found = foundStatic;
#endif
