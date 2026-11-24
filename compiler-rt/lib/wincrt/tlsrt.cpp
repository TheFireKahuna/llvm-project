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
// pages. A revival or a park names its actor in the chunk's entry.
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

// A page's control: an occupancy bit per id, 32 to a group word with TagA,
// set while A's return is in flight; a bit per id whose entry is in B; and
// the page's word, whether each slot's page is committed (CA, CB) or being
// returned (RA, RB), and the returner's client id.
struct Control {
  uint64_t Group[32];
  uint64_t BBits[16];
  uint64_t W;
  uint64_t Pad[15];
};
static_assert(sizeof(Control) == 512);
constexpr uint64_t TagA = uint64_t(1) << 32;
enum : uint64_t { CA = 1, CB = 2, RA = 4, RB = 8 };

uint32_t *slot(uint32_t Tid, bool B) {
  return reinterpret_cast<uint32_t *>(meta() + B * MapSpan + Tid);
}
void *pageOf(uint32_t Page, bool B) {
  return meta() + B * MapSpan + uintptr_t(Page) * wincrt::tls::Page;
}
Control &control(uint32_t Page) {
  return reinterpret_cast<Control *>(meta() + 2 * MapSpan)[Page];
}

// The control of a page, committed. Eight share a page of their own, which
// is committed once and kept.
Control *controlFor(uint32_t Page) {
  uint64_t *Bits = &shared().ControlCommitted[Page / 8 / 64];
  uint64_t Bit = uint64_t(1) << (Page / 8 % 64);
  if (!(load(Bits) & Bit)) {
    if (!commit(&control(Page & ~7u), wincrt::tls::Page))
      return nullptr;
    __atomic_fetch_or(Bits, Bit, __ATOMIC_ACQ_REL);
  }
  return &control(Page);
}

bool committed(uint32_t Page) {
  return load(&shared().ControlCommitted[Page / 8 / 64]) >> (Page / 8 % 64) & 1;
}

void evaluate(Record &Self, uint32_t Page);

void setCommitted(Control &C, uint64_t Bit) {
  __atomic_fetch_or(&C.W, Bit, __ATOMIC_ACQ_REL);
}

// The ids of the page with an entry in a slot: B by its bits, A by its
// occupancy. A plain load decides only to look further; a nonzero one, which
// could be stale, is confirmed by a read-modify-write.
bool holds(Control &C, bool B) {
  for (unsigned I = 0, N = B ? 16 : 32; I != N; ++I) {
    uint64_t *W = B ? &C.BBits[I] : &C.Group[I];
    uint64_t Mask = B ? ~uint64_t(0) : TagA - 1;
    if (load(W) & Mask && peek(W) & Mask)
      return true;
  }
  return false;
}

void releaseReturn(Record &Self, Control &C, uint64_t R, uint32_t Page) {
  __atomic_fetch_and(&C.W, ~(R | ~uint64_t(0) << 32), __ATOMIC_ACQ_REL);
  store(&Self.Return, 0);
  evaluate(Self, Page);
}

// Returns A's page: every group word is tagged from empty, stopping at an
// occupant; then the decommit, and A again where a publisher filled B
// meanwhile, so that its derivation reads zero rather than faulting. A tag
// already standing is a dead returner's, resumed.
void returnA(Record &Self, Control &C, uint32_t Page) {
  unsigned G = 0;
  for (; G != 32; ++G) {
    uint64_t Z = 0;
    if (!cas(&C.Group[G], Z, TagA) && Z != TagA)
      break;
  }
  if (G == 32) {
    decommit(pageOf(Page, false), wincrt::tls::Page);
    if (__atomic_fetch_and(&C.W, ~CA, __ATOMIC_ACQ_REL) & CB &&
        commit(pageOf(Page, false), wincrt::tls::Page))
      setCommitted(C, CA);
  }
  while (G--)
    __atomic_fetch_and(&C.Group[G], ~TagA, __ATOMIC_ACQ_REL);
  releaseReturn(Self, C, RA, Page);
}

void returnB(Record &Self, Control &C, uint32_t Page) {
  if (!holds(C, true)) {
    decommit(pageOf(Page, true), wincrt::tls::Page);
    __atomic_fetch_and(&C.W, ~CB, __ATOMIC_ACQ_REL);
  }
  releaseReturn(Self, C, RB, Page);
}

// Returns a slot of the page that holds no entry, B first, each by one
// compare-exchange that names the returner, journalled in its record.
void evaluate(Record &Self, uint32_t Page) {
  Control &C = control(Page);
  for (int B = 1; B >= 0; --B) {
    uint64_t W = load(&C.W);
    if (W & (RA | RB))
      return;
    if (!(W & (B ? CB : CA)) || holds(C, B))
      continue;
    store(&Self.Return, Page + 1);
    if (cas(&C.W, W, W | (B ? RB : RA) | uint64_t(currentTid()) << 32))
      return B ? returnB(Self, C, Page) : returnA(Self, C, Page);
    store(&Self.Return, 0);
    return;
  }
}

} // namespace

uint32_t wincrt::tls::lookup(uint32_t Tid) {
  uint32_t Page = Tid / wincrt::tls::Page;
  if (Page < Prefix)
    return load(slot(Tid, false));
  if (Page >= MapPages || !committed(Page))
    return 0;
  uint64_t W = load(&control(Page).W);
  uint32_t E = W & CA ? __wincrt_probe32(slot(Tid, false)) : 0;
  return E || !(W & CB) ? E : __wincrt_probe32(slot(Tid, true));
}

bool wincrt::tls::publish(uint32_t Tid, uint32_t Entry) {
  uint32_t Page = Tid / wincrt::tls::Page, Id = Tid / 4 % 1024;
  if (Page < Prefix) {
    store(slot(Tid, false), Entry);
    return true;
  }
  Control *C = Page < MapPages ? controlFor(Page) : nullptr;
  if (!C)
    return false;
  uint64_t Group = add(&C->Group[Id / 32], uint64_t(1) << Id % 32);
  bool InB = Group & TagA;
  // A's return is in flight: B, unless B's has begun since, which A's ended
  // before and Id's occupancy now blocks. A is committed beside B, so that
  // the derivation reads zero there.
  if (InB) {
    if (!commit(pageOf(Page, true), wincrt::tls::Page))
      return __atomic_fetch_and(&C->Group[Id / 32], ~(uint64_t(1) << Id % 32),
                                __ATOMIC_ACQ_REL),
             false;
    setCommitted(*C, CB);
    __atomic_fetch_or(&C->BBits[Id / 64], uint64_t(1) << Id % 64,
                      __ATOMIC_ACQ_REL);
    if (peek(&C->W) & RB) {
      __atomic_fetch_and(&C->BBits[Id / 64], ~(uint64_t(1) << Id % 64),
                         __ATOMIC_ACQ_REL);
      InB = false;
    }
  }
  if (!(peek(&C->W) & CA)) {
    if (commit(pageOf(Page, false), wincrt::tls::Page))
      setCommitted(*C, CA);
    else if (!InB)
      return __atomic_fetch_and(&C->Group[Id / 32], ~(uint64_t(1) << Id % 32),
                                __ATOMIC_ACQ_REL),
             false;
  }
  store(slot(Tid, InB), Entry);
  return true;
}

// The entry's owner writes it: the thread, or its finisher while its pin
// keeps the id. Its occupancy keeps A's page, and its B bit B's.
static bool inB(uint32_t Tid) {
  uint32_t Page = Tid / wincrt::tls::Page, Id = Tid / 4 % 1024;
  return Page >= Prefix && load(&control(Page).BBits[Id / 64]) >> (Id % 64) & 1;
}

bool wincrt::tls::replace(uint32_t Tid, uint32_t From, uint32_t To) {
  return cas(slot(Tid, inB(Tid)), From, To);
}

void wincrt::tls::clear(Record &Self, uint32_t Tid, uint32_t Entry) {
  uint32_t Page = Tid / wincrt::tls::Page, Id = Tid / 4 % 1024;
  if (Page >= Prefix && !committed(Page))
    return;
  uint32_t *E = slot(Tid, inB(Tid));
  uint32_t Was[] = {Entry, TP_TOMBSTONE};
  for (uint32_t V : Was)
    cas(E, V, 0);
  if (Page < Prefix)
    return;
  Control &C = control(Page);
  __atomic_fetch_and(&C.BBits[Id / 64], ~(uint64_t(1) << Id % 64),
                     __ATOMIC_ACQ_REL);
  __atomic_fetch_and(&C.Group[Id / 32], ~(uint64_t(1) << Id % 32),
                     __ATOMIC_ACQ_REL);
  evaluate(Self, Page);
}

// Resumes the return that a dead record's journal names, where its word
// still names the dead thread.
void wincrt::tls::resumeReturn(Record &Self, Record &Dead) {
  uint64_t Page = load(&Dead.Return);
  if (!Page--)
    return;
  Control &C = control(Page);
  uint64_t W = load(&C.W);
  if (W >> 32 == tidOf(Dead.Word) && W & (RA | RB) &&
      cas(&C.W, W, (W & 0xFFFFFFFF) | uint64_t(currentTid()) << 32)) {
    store(&Self.Return, Page + 1);
    W &RA ? returnA(Self, C, Page) : returnB(Self, C, Page);
  }
  store(&Dead.Return, 0);
}

//===----------------------------------------------------------------------===//
// Chunks and records
//===----------------------------------------------------------------------===//

// Plane P's run of a chunk: its slots 16 to 63, at P planes from the blocks.
void *wincrt::tls::planeRun(uint32_t C, unsigned P) {
  return chunkBase(C) + 16 * TP_BLOCK + P * TP_PLANE;
}

// Commits a chunk and every open plane, or answers false having committed
// nothing that stays.
static bool revive(uint32_t C) {
  Chunk &Ch = chunkAt(C);
  uint64_t Actor = 0, W = load(&Ch.Word);
  if ((W & ChunkState) != ChunkEmpty && (W & ChunkState) != ChunkParked)
    return false;
  if (!cas(&Ch.Actor, Actor, currentTid()))
    return false;
  bool Revived = cas(&Ch.Word, W, ChunkReviving);
  if (Revived) {
    // Read after the state's change: an opening plane either reads the
    // chunk reviving or has published its bit here.
    uint64_t Planes = peek(&shared().Planes);
    Revived = commit(chunkBase(C), 64 * TP_BLOCK);
    for (unsigned P = 1; Revived && P <= TP_PLANES; ++P)
      Revived = !(Planes >> P & 1) || commit(planeRun(C, P), TP_PLANE);

    if (!Revived)
      decommit(chunkBase(C), ChunkSize);
    store(&Ch.Word, Revived ? ChunkLive | FullBits : ChunkEmpty);
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
    uint64_t *R = &recordAt(C * 64 + Slot).Word, Old = load(R);
    if (stateOf(Old) != Free || !cas(R, Old, recordWord(Claimed, Tid, Old)))
      continue;
    uint64_t Mine = recordWord(Claimed, Tid, Old), W = load(&Ch.Word);
    while ((W & ChunkState) == ChunkLive && W >> Slot & 1)
      if (cas(&Ch.Word, W, W & ~(uint64_t(1) << Slot))) {
        store(&shared().Hint, C);
        return C * 64 + Slot;
      }
    // Its chunk is parking, which seals the record, or another took the bit.
    cas(R, Mine, recordWord(Free, 0, Mine));
  }
  return 0;
}

// Takes a slot of a live chunk, else revives a parked or returned one, else
// publishes a chunk never used, whose entry's page is committed first. Each
// pass that takes nothing follows another thread's take or publication, or
// this one's, and the chunks are bounded.
uint32_t wincrt::tls::claim(uint32_t Tid) {
  for (;;) {
    uint32_t Top = load(&shared().Top), Hint = load(&shared().Hint);
    for (uint32_t I = 0; I != Top; ++I)
      if (uint32_t Index = takeIn((Hint + I) % Top + 1, Tid))
        return Index;
    for (uint32_t I = 0; I != Top; ++I)
      if (revive(I + 1))
        if (uint32_t Index = takeIn(I + 1, Tid))
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
  if (cas(&Ch.Word, W, ChunkParking) || (W & ChunkState) == ChunkParking ||
      (W & ChunkState) == ChunkParked) {
    for (unsigned Slot = 16; Slot != 63; ++Slot) {
      uint64_t *R = &recordAt(C * 64 + Slot).Word, Old = load(R);
      if (stateOf(Old) != Free)
        cas(R, Old, recordWord(Free, 0, Old));
    }
    if (peek(&shared().Planes) & Change) {
      store(&Ch.Word, ChunkParked);
    } else {
      decommit(chunkBase(C), ChunkSize);
      store(&Ch.Word, ChunkEmpty);
    }
  }
  store(&Ch.Actor, 0);
}

void wincrt::tls::release(uint32_t Index) {
  Record &R = recordAt(Index);
  Chunk &Ch = chunkAt(Index / 64);
  uint64_t Bit = uint64_t(1) << Index % 64, C = load(&Ch.Word);
  // The bit, then the word: a record whose claimer died before its bit holds
  // none.
  bool Held = (C & ChunkState) == ChunkLive && !(C & Bit);
  for (uint64_t W = load(&R.Word);
       stateOf(W) != Free && !cas(&R.Word, W, recordWord(Free, 0, W));)
    ;
  if (Held && add(&Ch.Word, Bit) + Bit == (ChunkLive | FullBits))
    park(Index / 64);
}

bool wincrt::tls::advance(Record &R, uint64_t State) {
  for (uint64_t W = load(&R.Word);;) {
    if (stateOf(W) == Finishing || stateOf(W) == Free)
      return false;
    if (cas(&R.Word, W, recordWord(State, tidOf(W), W)))
      return true;
  }
}

//===----------------------------------------------------------------------===//
// Adoption
//===----------------------------------------------------------------------===//

// Zeroes the block and its slot of every open plane, then copies every
// registered image's template, again while a registration's generation moves:
// either the registration's walk reaches the block, or the seed reads the
// raised generation.
void wincrt::tls::seed(uint32_t Index) {
  char *Tp = blockAt(Index);
  for (uint64_t Gen = load(&shared().CreateGen);;
       Gen = load(&shared().CreateGen)) {
    uint64_t Planes = load(&shared().Planes);
    __builtin_memset(Tp, 0, TP_BLOCK);
    for (unsigned P = 1; P <= TP_PLANES; ++P)
      if (Planes >> P & 1)
        __builtin_memset(Tp + P * TP_PLANE, 0, TP_BLOCK);
    auto &H = *reinterpret_cast<Header *>(Tp);
    H.TcbSelf = H.Self = reinterpret_cast<uintptr_t>(Tp);
    seedImages(Index);
    if (load(&shared().CreateGen) == Gen)
      return;
  }
}

// Each resource is in the record before the next is taken: the client id by
// the claim, then the pin, the system call's output, then the block, so that
// a kill anywhere leaves the record what the rotating check finishes. The
// TID map's entry, published last, is the adoption's linearization point.
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
  bool Adopted =
      NtDuplicateObject(GetCurrentProcess(), Thread, GetCurrentProcess(),
                        &R.Pin, SYNCHRONIZE, 0, 0) >= 0;
  if (Adopted) {
    seed(Index);
    Adopted = advance(R, Live) && publish(Tid, Index);
  }
  // A refused adoption leaves its record to the thread's finisher.
  if (!Adopted) {
    advance(R, Releasing);
    return -EAGAIN;
  }
  return reinterpret_cast<intptr_t>(blockAt(Index));
}

Header *wincrt::tls::header(bool Adopt) {
  uint32_t Entry = lookup(currentTid());
  if (Entry & TP_SLOT_TEST)
    return reinterpret_cast<Header *>(blockAt(Entry & TP_MASK));
  intptr_t Tp = Adopt && Entry != TP_TOMBSTONE
                    ? adopt(currentTid(), GetCurrentThread())
                    : -1;
  return Tp < 0 ? nullptr : reinterpret_cast<Header *>(Tp);
}

//===----------------------------------------------------------------------===//
// Thread end
//===----------------------------------------------------------------------===//

// Every step is idempotent, and keeps where it is when run again: the map's
// entry moves by compare-exchange from the value it replaces.
// A dead protector's protection ends with its hold, and a dead finisher's
// with the finish it left.
static void finish(Record &Self, uint32_t Index, unsigned How) {
  Record &R = recordAt(Index);
  uint32_t Tid = tidOf(load(&R.Word)), Entry = Index;

  if (How & StepDrop)
    dropThread(reinterpret_cast<Header *>(blockAt(Index))->Thread);
  if (How & StepTomb)
    cas(slot(Tid, inB(Tid)), Entry, TP_TOMBSTONE);
}

void wincrt::tls::releaseSelf() {
  uint32_t Entry = lookup(currentTid()), Index = Entry & TP_MASK;
  if (!(Entry & TP_SLOT_TEST) || !advance(recordAt(Index), Releasing))
    return;
  Record &R = recordAt(Entry & TP_MASK);
  if (!advance(R, Releasing))
    return;
  finish(R, Entry & TP_MASK, Release);
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
               : reinterpret_cast<uintptr_t>(area()) +
                     TP_CLOSED_SLOT * TP_BLOCK;
  // Only the executable's code runs before its own fill: a DLL's entry that
  // finds its map unwritten fails closed.
  auto *Exe = static_cast<char *>(
      NtCurrentTeb()->ProcessEnvironmentBlock->Reserved3[1]);
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
    __wincrt_probe_landing[];

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
  DWORD64 &Pc = Exception->ContextRecord->Rip;
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
  void *Page = const_cast<uint64_t *>(Word);
  SIZE_T Size = sizeof(uint64_t);
  ULONG Old;
  if (NtProtectVirtualMemory(GetCurrentProcess(), &Page, &Size, PAGE_READWRITE,
                             &Old) < 0)
    return false;
  // The word may be a constant to the compiler, which the empty asm hides it
  // from.
  auto *To = const_cast<uint64_t *>(Word);
  __asm__("" : "+r"(To) : : "memory");
  *To = Value;
  return NtProtectVirtualMemory(GetCurrentProcess(), &Page, &Size,
                                PAGE_READONLY, &Old) >= 0 &&
         *Word == Value;
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
  const char *Names[] = {"__tp_map", "__tp_base", "__tp_slow",
                         "__tp_register"};
  for (const char *Name : Names)
    Trusted = Trusted && GetProcAddress(Other, Name);

  UnmapViewOfFile(View);
  CloseHandle(Section);
  if (!Trusted)
    return -1;
  auto Word = [&](const char *Name) {
    return *reinterpret_cast<const uintptr_t *>(GetProcAddress(Other, Name));
  };
  return seal(Word("__tp_map"), Word("__tp_base")) ? 0 : -1;
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
  shared().Taken[0] = (uint64_t(1) << TP_EXE_PLACE / Granule) - 1;
  return AddVectoredExceptionHandler(1, recoverProbe) && foundImages() &&
         (findTail(), true);
}

#ifndef COMPILER_RT_SHARED_LIB
// Under -static the executable holds the runtime, founded by its first TLS
// callback, before its entry point and before the fill's callback, which
// .CRT$XLAF holds.
static void NTAPI foundStatic(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_ATTACH && !found())
    wincrt::fatal("the thread-local runtime cannot be founded");
}
#pragma section(".CRT$XLAA", read)
extern "C" __declspec(allocate(".CRT$XLAA"))
const PIMAGE_TLS_CALLBACK __wincrt_tls_found = foundStatic;
#endif
