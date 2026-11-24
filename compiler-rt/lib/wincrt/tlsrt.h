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

// The runtime never derives a thread pointer inside itself: its entries and
// what its assembly calls run with none.
#if __has_attribute(thread_pointer)
#define WINCRT_NO_TP __attribute__((thread_pointer("none")))
#else
#define WINCRT_NO_TP
#endif

extern "C" {
// Exported by ntdll; no SDK header declares them.
NTSYSAPI NTSTATUS NTAPI NtAllocateVirtualMemoryEx(HANDLE, PVOID *, SIZE_T *,
                                                  ULONG, ULONG,
                                                  MEM_EXTENDED_PARAMETER *,
                                                  ULONG);
NTSYSAPI NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
NTSYSAPI NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG,
                                               ULONG *);
NTSYSAPI NTSTATUS NTAPI NtDuplicateObject(HANDLE, HANDLE, HANDLE, PHANDLE,
                                          ACCESS_MASK, ULONG, ULONG);
NTSYSAPI NTSTATUS NTAPI NtOpenThread(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                     CLIENT_ID *);

// The no-thread header: what the thread pointer names on a thread with no
// block. Read-only, its thread-pointer words naming itself.
WINCRT_TLS_API extern const uint64_t __tp_none[TP_H / 8];
}

#define STATUS_INVALID_CID ((NTSTATUS)0xC000000BL)
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
template <typename T> T peek(T *P) { return add(P, 0); }

// A record: the tenancy of one slot's block, its word {state, client id,
// generation}, which moves only by compare-exchange.
struct Record {
  uint64_t Word;
  HANDLE Pin;      // the thread's own handle, which keeps its client id
  uint64_t Return; // one plus the map page whose return the thread runs

  // One plus the record a check of the thread's protects, with Landed once
  // the protection stands.
  uint64_t Checked;
  uint64_t Pad[4];
};
static_assert(sizeof(Record) == 64);
enum : uint64_t { Free, Claimed, Live, Releasing, Finishing };
// The record's protectors: a check that waits on its pin, and a registration
// that writes its block, each of which keeps it from being finished, so that
// its pin stays open and its chunk does not park; only a check that is the
// sole protector elects its finisher. The record's own thread moves its word
// over them.
constexpr uint64_t Protect = uint64_t(1) << 56, Protectors = uint64_t(15) << 56,
                   Landed = uint64_t(1) << 63;
inline uint64_t stateOf(uint64_t W) { return W >> 60; }
inline uint32_t tidOf(uint64_t W) { return uint32_t(W >> 32 & 0xFFFFFF) << 2; }
inline uint64_t recordWord(uint64_t State, uint32_t Tid, uint64_t Old) {
  return State << 60 | (Old & Protectors) | uint64_t(Tid >> 2) << 32 |
         uint32_t(Old + 1);
}

// A chunk's resident entry. Word holds the free bits of slots 16 to 62 and
// the state; Actor the client id of the thread whose revival or park is in
// flight.
struct Chunk {
  uint64_t Word;
  uint64_t Actor;
};
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
  uint64_t Taken[3]; // the granules of the block's, plane 1's and plane 2's
  void *Set;         // the image set's first page
};
constexpr uint32_t StateVersion = 1;
constexpr uintptr_t StateAt =
    2 * MapSpan + MapPages * 512 + Chunks * sizeof(Chunk);
constexpr uint64_t Change = uint64_t(1) << 63;

// The runtime's exports, read-only once founding has written them: A, then
// B, the map's control blocks and the chunk entries; the block area.
extern "C" WINCRT_TLS_API const volatile uintptr_t __tp_map, __tp_base;
inline char *meta() { return reinterpret_cast<char *>(__tp_map); }
inline char *area() { return reinterpret_cast<char *>(__tp_base); }
inline State &shared() { return *reinterpret_cast<State *>(meta() + StateAt); }

inline char *blockAt(uint32_t Index) {
  return area() + (uintptr_t(Index >> 6) << TP_CHUNK_SHIFT) +
         (uintptr_t(Index & 63) << TP_BLOCK_SHIFT);
}
inline Chunk &chunkAt(uint32_t C) {
  return reinterpret_cast<Chunk *>(meta() + 2 * MapSpan + MapPages * 512)[C];
}
inline char *chunkBase(uint32_t C) {
  return area() + (uintptr_t(C) << TP_CHUNK_SHIFT);
}
inline Record &recordAt(uint32_t Index) {
  return reinterpret_cast<Record *>(
      area() +
      (uintptr_t(Index >> 6) << TP_CHUNK_SHIFT))[(Index & 63) - 16];
}
inline uint32_t currentTid() {
  return *reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const char *>(NtCurrentTeb()) + TP_CLIENT_ID);
}

// Commits or decommits [Base, Base + Size).
bool commit(void *Base, uintptr_t Size);
void decommit(void *Base, uintptr_t Size);

// The map: the entry of a client id, its first publication, the replacement
// of the entry it holds by a compare-exchange, and its clear.
uint32_t lookup(uint32_t Tid);
bool publish(uint32_t Tid, uint32_t Entry);
bool replace(uint32_t Tid, uint32_t From, uint32_t To);
void clear(Record &Self, uint32_t Tid, uint32_t Entry);
void resumeReturn(Record &Self, Record &Dead);

// Claims a record for the calling thread, or answers 0; frees one, parking
// its chunk where that frees the chunk's last; parks a chunk; finishes the
// revival or park of a chunk whose actor is gone.
uint32_t claim(uint32_t Tid);
void release(uint32_t Index);
void park(uint32_t C);
void finishChunk(uint32_t C);
void *planeRun(uint32_t C, unsigned P);
// Whether the thread with client id Tid, whose handle no record holds, is
// past its last instruction: its id names no thread, or a signalled one.
bool gone(uint32_t Tid);

// The thread pointer of the thread with client id Tid, whose handle Thread
// is, adopting it where it has no block: the calling thread, or one the
// caller created suspended. A negative errno where it is refused.
intptr_t adopt(uint32_t Tid, HANDLE Thread);
// The calling thread's block header, adopting the thread where Adopt is set;
// null where it has none, or it is released.
Header *header(bool Adopt);
// Moves the caller's own record to State, over its protectors.
bool advance(Record &R, uint64_t State);
// The steps that end a record, in this order; each caller selects some. The
// release at the thread's last hook takes Release, its finisher Finish.
enum : unsigned {
  StepDrop = 2,   // drop its destructor list without running it
  StepTomb = 4,   // replace its TID-map entry with the tombstone
  StepClear = 8,  // clear its TID-map entry
  StepScan = 16,  // drop the protection a dead check of its own left
  StepClose = 32, // close its pin
  StepFree = 64,  // free the record and its block
  Release = StepDrop | StepTomb,
  Finish = StepDrop | StepClear | StepScan | StepClose | StepFree
};
void finish(Record &Self, uint32_t Index, unsigned How);
// The release at the calling thread's last hook.
void releaseSelf();
// The rotating check: finishes the next record or two whose thread is gone.
void check(Record &Self);
// Adds a protector to a record, unless it is free or being finished; drops
// one.
bool protect(Record &R);
void unprotect(Record &R);
// Drops a thread's destructor list without running it.
void dropThread(ThreadState &T);
// Finds whether the executable's last TLS callback is __wincrt_thread_end.
void findTail();
// Seeds the block of record Index before its thread runs code of the model.
void seed(uint32_t Index);

// Writes every registered image's template into the block of record Index,
// being seeded.
void seedImages(uint32_t Index);
// Registers the executable, and has the loader notify unloads.
bool foundImages();
// Writes a word of read-only image memory under the fill's bracket.
bool bracket(volatile const uint64_t *Word, uint64_t Value);

// Founds the process's runtime, or finds the one another copy founded.
bool found();

// Accesses to pages the runtime may return, or images being unloaded, which
// find zero there: a u32's and a word's load; a copy of whole words, false
// where it stopped; a word's compare-exchange, answering the word it found.
extern "C" {
uint32_t __wincrt_probe32(const void *);
uint64_t __wincrt_probe64(const void *);
bool __wincrt_probe_copy(void *, const void *, size_t);
uint64_t __wincrt_probe_cas(uint64_t *, uint64_t, uint64_t);
}

} // namespace wincrt::tls

#endif // COMPILER_RT_LIB_WINCRT_TLSRT_H
