//===-- exit_state.h - The process's exit-list state ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The state of the termination registries, which every executable of ours
// reserves and every copy of the registries in the process uses: the
// clang_rt.wincrt_dynamic.dll that most images import from, a second one
// loaded by full path or under another name, and the copy that -static links
// into an image. One list orders every image's registrations, so exit runs
// them in exact reverse order across copies, under the one Universal CRT token
// the executable's start-up posts. A copy finds the state through
// PEB->ImageBaseAddress and the {tag, rva} that the linker writes at 0x30 in
// the executable's MZ header; without it, under a foreign executable, each
// copy keeps a state of its own.
//
// Every copy reads and writes these structures, so any change to them changes
// ExitStateVersion.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_EXIT_STATE_H
#define COMPILER_RT_LIB_WINCRT_EXIT_STATE_H

#include "wincrt.h"

namespace wincrt {

constexpr uint32_t ExitStateLocator = 0x30;
constexpr uint32_t ExitStateTag = 0x7E1A5C03;
constexpr uint32_t ExitStateVersion = 1;

// Chunk K of a record holds ExitFirstChunkEntries << K entries, so
// ExitMaxChunks chunks address every 32-bit slot.
constexpr uint32_t ExitFirstChunkEntries = 64;
constexpr unsigned ExitMaxChunks = 26;

// A registration of __cxa_atexit or __cxa_at_quick_exit.
struct ExitEntry {
  uintptr_t Dtor; // encoded with the state's cookie
  void *Obj;
  // Twice the global sequence number, plus one if Dtor was registered as
  // carrying the salted destructor type.
  uint64_t Seq;
  uint32_t Slot;
  uint32_t Next;
};

// The registrations of one image, with an arena of entries and lock-free
// stacks over them. The heads are {tag, slot} words.
struct ExitImage {
  // The registering image's handle, RetiredDso from its detach until the
  // record is released, and null while the record is free.
  void *Dso;
  uint64_t Live; // stack of pending entries
  uint64_t Free; // stack of consumed slots
  uint32_t Bump; // next slot never used
  ExitEntry *Chunks[ExitMaxChunks];
  ExitImage *Next; // every record, append-only
  ExitImage *NextRetired;
  uint32_t RetiredIn; // the drainer generation of its retirement
  // The registering image's base and size, once a registration has looked
  // them up; Size is zero until then.
  uintptr_t Begin;
  uintptr_t Size;
};

struct ExitRegistry {
  ExitImage *Images;
  ExitImage *Hint; // the record found last
  ExitImage *Retired;
  LONG Token;
};

// A thread's thread-local destructors, and the images they keep loaded.
constexpr uint32_t ThreadExitBlockEntries = 32;

struct ThreadExitEntry {
  uintptr_t Dtor; // encoded with the state's cookie, null once run
  void *Obj;
  void *Dso;
};

struct ThreadExitBlock {
  ThreadExitEntry Entries[ThreadExitBlockEntries];
  ThreadExitBlock *Next;
  uint32_t Count;
  // Bit I is set if Entries[I] was registered as carrying the salted
  // destructor type.
  uint32_t Salted;
};

struct ThreadExitModule {
  HMODULE Handle; // null for the executable, which is never unloaded
  uintptr_t Begin;
  uintptr_t Size;
  ThreadExitModule *Next;
};

struct ThreadExitState {
  ThreadExitBlock *Blocks;
  ThreadExitModule *Modules;
  bool Finalizing;
};

struct ExitState {
  uint32_t Version;
  uint32_t Size;
  // One plus the TLS slot that holds each thread's ThreadExitState, which
  // the first registration of a thread-local destructor allocates.
  LONG ThreadSlot;
  // The key that registered functions are encoded with: the security cookie
  // of the first copy to register one.
  uintptr_t Cookie;
  ExitRegistry Normal, Quick;
  uint64_t Sequence;
  // Drainers hold records' arenas alive, by epochs; see cxa_atexit.cpp.
  uint32_t Epoch;
  LONG Drainers[2];
  LONG ExecutableRegistered;
  LONG Drained;
  uintptr_t AfterDrain; // encoded
};

// The process's exit-list state: the executable's, or this copy's own under
// an executable that reserves none.
ExitState &exitState();
// Whether exitState() is the executable's.
bool isProcessExitState();
// The calling thread's thread-local destructors, created if Create is set;
// null if there are none, or if they cannot be created.
ThreadExitState *threadExitState(bool Create);
// Ends the calling thread's thread-local destructors.
void releaseThreadExitState(ThreadExitState *State);

inline uintptr_t exitCookie(ExitState &State) {
  uintptr_t Cookie = __atomic_load_n(&State.Cookie, __ATOMIC_ACQUIRE);
  if (Cookie)
    return Cookie;
  uintptr_t Expected = 0;
  if (__atomic_compare_exchange_n(&State.Cookie, &Expected,
                                  __security_cookie | 1, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return __security_cookie | 1;
  return Expected;
}

inline uintptr_t encodeExit(ExitState &State, Destructor Function) {
  uintptr_t Cookie = exitCookie(State);
  return __builtin_rotateright64(reinterpret_cast<uintptr_t>(Function) ^ Cookie,
                                 Cookie & 63);
}

inline Destructor decodeExit(ExitState &State, uintptr_t Value) {
  uintptr_t Cookie = exitCookie(State);
  return reinterpret_cast<Destructor>(
      __builtin_rotateleft64(Value, Cookie & 63) ^ Cookie);
}

} // namespace wincrt

#endif // COMPILER_RT_LIB_WINCRT_EXIT_STATE_H
