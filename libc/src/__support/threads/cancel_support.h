//===-- Platform-neutral thread cancellation support -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides the cancellation check hot path and constants used by
// pthread_cancel, pthread_setcancelstate, pthread_setcanceltype, and all
// cancellation points. Platform-specific implementations supply the per-thread
// cancel word access and the forced-unwind cold path.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_CANCEL_SUPPORT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_CANCEL_SUPPORT_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "include/llvm-libc-types/__pthread_cleanup_t.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace cancel {

// Cancel word bit layout. Packed into a single uint8_t for a single-load
// check at every cancellation point (matches glibc's pattern).
//
//   Bit 0: state    — 0 = ENABLE, 1 = DISABLE
//   Bit 1: type     — 0 = DEFERRED, 1 = ASYNCHRONOUS
//   Bit 2: pending  — 0 = none, 1 = cancellation requested
inline constexpr uint8_t STATE_BIT = 0x01;
inline constexpr uint8_t TYPE_BIT = 0x02;
inline constexpr uint8_t PENDING_BIT = 0x04;

// Fast-path mask: (word & CHECK_MASK) == PENDING_BIT means
// "enabled AND pending" in one compare.
inline constexpr uint8_t CHECK_MASK = STATE_BIT | PENDING_BIT;

// Returns a pointer to the current thread's cancel word, or nullptr if the
// calling thread has no cancel state (e.g. the main thread before
// pthread_create, or a foreign thread). Platform-specific.
cpp::Atomic<uint8_t> *get_cancel_word();

// Cold path: initiate forced unwind for cancellation. Fires C++ destructors,
// walks pthread_cleanup_push chain, exits thread. Does not return.
// Implemented in __support/threads/cancel_unwind.cpp using _Unwind_ForcedUnwind
// (Itanium ABI §1.6.4) — portable across all Itanium-ABI targets.
[[noreturn]] void act();

// Cold path: initiate forced unwind for pthread_exit. Same unwind machinery
// as act(), but sets the return value instead of PTHREAD_CANCELED.
[[noreturn]] void exit_with_unwind(void *retval);

// Platform hook called by act() before initiating the forced unwind. Allows
// the platform to cancel in-flight I/O or perform other cleanup that must
// happen while stack frames are still live. Default (non-Windows): no-op.
void before_unwind();

// Hot path: check for pending cancellation. Inlined at every cancellation
// point. Single atomic load — returns immediately if no cancellation or if
// the thread has no cancel state.
LIBC_INLINE void check() {
  auto *word = get_cancel_word();
  if (LIBC_LIKELY(!word))
    return;
  uint8_t val = word->load(cpp::MemoryOrder::ACQUIRE);
  if (LIBC_LIKELY((val & CHECK_MASK) != PENDING_BIT))
    return;
  act();
}

// Request cancellation of the thread identified by |tid|. Sets PENDING_BIT
// on the target's cancel word. For asynchronous cancellation, arranges
// prompt delivery (e.g. APC on Windows, SIGCANCEL on Linux). For self-cancel
// (tid matches current thread), sets the bit and calls check() inline.
// Returns 0 on success, ESRCH if the thread is not found. Platform-specific.
int request(int tid);

// Cleanup stack management. Platform-specific implementations maintain
// the per-thread cleanup handler chain (linked list of __pthread_cleanup_t).
// Uses the global-namespace type — must match ThreadLifecycle::cancel_cleanup_stack.
::__pthread_cleanup_t *get_cleanup_head();
void set_cleanup_head(::__pthread_cleanup_t *frame);

} // namespace cancel
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_CANCEL_SUPPORT_H
