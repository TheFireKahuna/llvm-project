//===-- tlsrt.h - The owned thread-local runtime's internals ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The process's thread-local runtime: the TID map, the block area of 256 KiB
// chunks, each holding the records of its slots 16 to 62, their 1 KiB blocks
// and four planes, and the records' claim and release. Nothing here takes a
// lock or waits: every shared word moves by one atomic step, and a step a
// kill interrupts is left where a finisher, the rotating check, reads it.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_TLSRT_H
#define COMPILER_RT_LIB_WINCRT_TLSRT_H

#include "wincrt.h"

#include "tls/ThreadPointer.inc"

// The runtime's exports, which every image that holds a copy of it exports,
// so that a later copy can trust it (tlsrt.cpp).
#define WINCRT_TLS_API __declspec(dllexport)

extern "C" {
// Exported by ntdll; no SDK header declares them.
NTSYSAPI NTSTATUS NTAPI NtAllocateVirtualMemoryEx(HANDLE, PVOID *, SIZE_T *,
                                                  ULONG, ULONG,
                                                  MEM_EXTENDED_PARAMETER *,
                                                  ULONG);
NTSYSAPI NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
NTSYSAPI NTSTATUS NTAPI NtDuplicateObject(HANDLE, HANDLE, HANDLE, PHANDLE,
                                          ACCESS_MASK, ULONG, ULONG);
NTSYSAPI NTSTATUS NTAPI NtOpenThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                     CLIENT_ID *);
NTSYSAPI NTSTATUS NTAPI NtSetInformationObject(HANDLE, ULONG, PVOID, ULONG);
NTSYSAPI NTSTATUS NTAPI NtCreateSection(PHANDLE, ACCESS_MASK,
                                        POBJECT_ATTRIBUTES, PLARGE_INTEGER,
                                        ULONG, ULONG, HANDLE);
// The no-thread header: what the thread pointer names on a thread with no
// block. Read-only, its thread-pointer words naming itself.
WINCRT_TLS_API extern const uint64_t __tp_none[TP_H / 8];
}

#define STATUS_INVALID_CID ((NTSTATUS)0xC000000BL)
#define SEC_NO_CHANGE 0x00400000
#define OBJ_PROTECT_CLOSE 0x00000001

namespace wincrt::tls {

constexpr uintptr_t Page = 0x1000;
constexpr uintptr_t ChunkSize = uintptr_t(1) << TP_CHUNK_SHIFT;
constexpr uint32_t Chunks = (TP_MASK + 1) >> 6;
// The client ids the map covers, a multiple of 4 below 2^26, each at its own
// offset; B, the second slot, follows A.
constexpr uintptr_t MapSpan = uintptr_t(1) << 26;
constexpr uint32_t MapPages = MapSpan / Page;
// The map's pages below 64 KiB, the client ids below 65536, are committed at
// founding and never returned.
constexpr uint32_t Prefix = 16;
constexpr uintptr_t Granule = 16;

template <typename T> T load(const T *P) {
  return __atomic_load_n(P, __ATOMIC_ACQUIRE);
}
template <typename T, typename U> void store(T *P, U V) {
  __atomic_store_n(P, static_cast<T>(V), __ATOMIC_RELEASE);
}
template <typename T, typename U> bool cas(T *P, T &Expected, U Desired) {
  return __atomic_compare_exchange_n(P, &Expected, static_cast<T>(Desired),
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
template <typename T, typename U> T add(T *P, U V) {
  return __atomic_fetch_add(P, static_cast<T>(V), __ATOMIC_ACQ_REL);
}
template <typename T, typename U> T setBits(T *P, U V) {
  return __atomic_fetch_or(P, static_cast<T>(V), __ATOMIC_ACQ_REL);
}
template <typename T, typename U> T clearBits(T *P, U V) {
  return __atomic_fetch_and(P, ~static_cast<T>(V), __ATOMIC_ACQ_REL);
}
template <typename T> T peek(T *P) { return add(P, 0); }
inline uint64_t run(unsigned N) {
  return N >= 64 ? ~uint64_t(0) : (uint64_t(1) << N) - 1;
}

// A reservation in flight: the system call's output, and what a release
// passes back with its size zero.
struct Reserve {
  void *Base;
  SIZE_T Size;
};
// Where a thread keeps the reservation it makes until its home holds it,
// and the one it releases until the kernel has written the size back: every
// reservation is created into a journal, so that a kill at any point leaves
// one a finisher can settle.
struct Journal {
  Reserve Making;
  uint64_t *Home;
  Reserve Freeing;
};

// A record: the tenancy of one slot's block, its word {state, client id,
// generation}, which moves only by compare-exchange.
struct Record {
  uint64_t Word;
  HANDLE Pin;      // the thread's own handle, which keeps its client id
  uint64_t Hold;   // the wide era a step of this thread acts under, or 0
  uint64_t Return; // one plus the map page whose return the thread runs
  Journal J;
  // One plus the record the thread protects, or the first slot of the chunk
  // all of whose records it protects: one a check waits on the pin of, or a
  // registration writes the blocks of. No record is finished while a live
  // thread protects it, so its pin stays open and its chunk does not park; a
  // dead protector's own finisher clears it.
  uint64_t Protecting;
  // The pin's thread's creation time, written by its closer before it tags
  // the pin closing, so that a rerun knows the pin's thread from another.
  uint64_t PinTime;
  uint64_t Pad[5];
};
static_assert(sizeof(Record) == 128);
enum : uint64_t { Free, Claimed, Live, Releasing, Finishing };
inline uint64_t stateOf(uint64_t W) { return W >> 60; }
inline uint32_t tidOf(uint64_t W) { return uint32_t(W >> 32 & 0xFFFFFF) << 2; }
inline uint64_t recordWord(uint64_t State, uint32_t Tid, uint64_t Old) {
  return State << 60 | uint64_t(Tid >> 2) << 32 | uint32_t(Old + 1);
}

// A chunk's resident entry. Word holds the free bits of slots 16 to 62 and
// the state; Actor the client id of the thread whose revival or park is in
// flight, which acts under Hold and makes and releases through J.
struct Chunk {
  uint64_t Word;
  uint64_t Actor;
  uint64_t Hold;
  Journal J;
};
static_assert(sizeof(Chunk) == 64);
enum : uint64_t {
  ChunkEmpty,
  ChunkReviving,
  ChunkLive,
  ChunkParking,
  ChunkParked,
  ChunkState = 7
};
constexpr uint64_t FullBits = 0x7FFFFFFFFFFF0000;

struct Dtor;
struct Module;
// A thread's thread-local destructors, newest first, the images they keep
// loaded, and whether its completion runs.
struct ThreadState {
  Dtor *Dtors;
  Module *Modules;
  bool Finalizing;
};

// The block's header: the words outside code addresses, then the runtime's.
struct Header {
  uintptr_t TcbSelf;
  uint64_t Reserved0;
  uintptr_t Self;
  uint64_t Reserved1[3];
  uint64_t PointerGuard;
  ThreadState Thread;
  void *Started; // the record of the thread's start, for a thread wincrt starts
};
static_assert(offsetof(Header, Self) == TP_SELF &&
              offsetof(Header, PointerGuard) == TP_POINTER_GUARD &&
              sizeof(Header) <= TP_H);

// The process's runtime, which every copy of it in the process shares: it
// lives behind the chunk entries in the runtime's reservation.
struct State {
  uint32_t Version, Size;
  uint32_t Top; // the chunks ever published
  uint32_t Hint;
  uint64_t Planes; // the open planes, and Change while one opens or closes
  uint64_t CreateGen;
  uint64_t Cursor; // the rotating check's
  // What thread-local destructors are encoded with: the founding copy's
  // security cookie, since each copy's differs.
  uint64_t Cookie;
  uint64_t ControlCommitted[MapPages / 8 / 64];
  uint64_t Taken[3];   // the granules of the block's, plane 1's and plane 2's
  void *Set;           // the image set's first page
  uint64_t Wide;       // the wide area's words taken, the executable's first
  uint64_t Era;        // the wide era, from 1
  uint64_t Holders;    // the set's wide and overflow entries
  uint64_t Pending;    // the set's entries removing or closing
  const uint64_t *Key; // the key's read-only page
};
constexpr uint32_t StateVersion = 1;
constexpr uintptr_t StateAt = 2 * MapSpan + MapPages * 512 + Chunks * 64;
constexpr uint64_t Change = uint64_t(1) << 63;

// The runtime's exports, read-only once founding has written them: A, then
// B, the map's control blocks and the chunk entries; the block area.
extern "C" WINCRT_TLS_API const volatile uintptr_t __tp_map, __tp_base,
    __tp_key;
inline char *meta() { return reinterpret_cast<char *>(__tp_map); }
inline char *area() { return reinterpret_cast<char *>(__tp_base); }
inline State &shared() { return *reinterpret_cast<State *>(meta() + StateAt); }

inline char *chunkBase(uint32_t C) {
  return area() + (uintptr_t(C) << TP_CHUNK_SHIFT);
}
inline char *blockAt(uint32_t Index) {
  return chunkBase(Index >> 6) + (uintptr_t(Index & 63) << TP_BLOCK_SHIFT);
}
inline Chunk &chunkAt(uint32_t C) {
  return reinterpret_cast<Chunk *>(meta() + 2 * MapSpan + MapPages * 512)[C];
}
inline Record &recordAt(uint32_t Index) {
  return reinterpret_cast<Record *>(chunkBase(Index >> 6))[(Index & 63) - 16];
}
// Plane P's run of a chunk: its slots 16 to 63, at P planes from the blocks.
inline char *planeRun(uint32_t C, unsigned P) {
  return chunkBase(C) + 16 * TP_BLOCK + P * TP_PLANE;
}
inline uint32_t currentTid() {
  return *reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const char *>(NtCurrentTeb()) + TP_CLIENT_ID);
}
inline char *executable() {
  return static_cast<char *>(
      NtCurrentTeb()->ProcessEnvironmentBlock->Reserved3[1]);
}

// Every live chunk with its claimed slots, by its bits; every claimed slot.
template <typename F> void eachChunk(F Each) {
  for (uint32_t C = 1, Top = load(&shared().Top); C <= Top; ++C)
    if (uint64_t W = load(&chunkAt(C).Word); (W & ChunkState) == ChunkLive)
      Each(C, ~W & FullBits);
}
template <typename F> void eachRecord(F Each) {
  eachChunk([&](uint32_t C, uint64_t Taken) {
    for (; Taken; Taken &= Taken - 1)
      Each(C * 64 + __builtin_ctzll(Taken));
  });
}

// Publishes Self's protection of record Index, or, for the first slot of a
// chunk, of every record of the chunk; ends it.
inline void shield(Record &Self, uint32_t Index) {
  __atomic_exchange_n(&Self.Protecting, uint64_t(Index) + 1, __ATOMIC_SEQ_CST);
}
inline void unprotect(Record &Self) { store(&Self.Protecting, 0); }

// Commits or decommits [Base, Base + Size).
bool commit(void *Base, uintptr_t Size);
void decommit(void *Base, uintptr_t Size);
// The entry of a client id in the TID map.
uint32_t lookup(uint32_t Tid);
// Parks a chunk whose last bit the caller freed.
void park(uint32_t C);

// The thread pointer of the thread with client id Tid, whose handle Thread
// is, adopting it where it has no block: the calling thread, or one the
// caller created suspended. A negative errno where it is refused.
intptr_t adopt(uint32_t Tid, HANDLE Thread);
// The calling thread's block header, adopting the thread where Adopt is set;
// null where it has none, or it is released.
Header *header(bool Adopt);
// The release at the calling thread's last hook.
void releaseSelf();
// Drops a thread's destructor list without running it.
void dropThread(ThreadState &T);
// Finds whether the executable's last TLS callback is __wincrt_thread_end.
void findTail();

// Writes every registered image's template into the block of record Index,
// being seeded; false where a reservation or a commit is refused.
bool seedImages(uint32_t Index);
// Registers the executable, and has the loader notify unloads.
bool foundImages();
// The wide and overflow entries' part of a chunk's revival and park, made
// by the chunk's actor; and of a record's end: its wide slots of a page or
// more decommitted, its hold cleared and the scan run.
bool reviveHook(uint32_t C);
void parkHook(uint32_t C);
void endWide(Record &Self, uint32_t Index);
void endHold(Record &Self, Record &Dead);
// Settles the journal of a maker that is gone.
void settle(Journal &J);
// Writes a word of read-only image memory under the fill's bracket.
bool bracket(volatile const uint64_t *Word, uint64_t Value);

// Founds the process's runtime, or finds the one another copy founded.
bool found();

// Accesses to pages the runtime may return, or images being unloaded, which
// find zero there: a u32's and a word's load; a copy of one or more whole
// words, false where it stopped; a word's compare-exchange, answering the
// word it found.
extern "C" {
WINCRT_NO_TP uint32_t __wincrt_probe32(const void *);
WINCRT_NO_TP uint64_t __wincrt_probe64(const void *);
WINCRT_NO_TP bool __wincrt_probe_copy(void *, const void *, size_t);
WINCRT_NO_TP uint64_t __wincrt_probe_cas(uint64_t *, uint64_t, uint64_t);
}
inline uint64_t probeLoad(const uint64_t *W) { return __wincrt_probe64(W); }

} // namespace wincrt::tls

#endif // COMPILER_RT_LIB_WINCRT_TLSRT_H
