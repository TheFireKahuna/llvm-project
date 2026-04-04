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

// Forward decl — full definition in src/__support/threads/thread.h.
// Pulled forward so cancel::request can take a typed pointer without
// dragging the full pthread surface into every cold-path TU.
struct ThreadAttributes;

namespace cancel {

// Cancel state bit layout. Stored in ThreadLifecycle::cancel_state (uint8_t).
// The pending notification lives separately in notify_word (notify::CANCEL).
//
//   Bit 0: state    — 0 = ENABLE, 1 = DISABLE
//   Bit 1: type     — 0 = DEFERRED, 1 = ASYNCHRONOUS
inline constexpr uint8_t STATE_BIT = 0x01;
inline constexpr uint8_t TYPE_BIT = 0x02;

// Returns a pointer to the current thread's cancel state byte, or nullptr
// if the calling thread has no cancel state. Used by cold paths
// (disable_cancel, exit_with_unwind) that need the state/type bits.
// Platform-specific.
cpp::Atomic<uint8_t> *get_cancel_word();

// Hot path: returns true if cancellation is both pending AND enabled.
// On Windows, this reads the ThreadLocalWord notify_word (single RELAXED
// load — plain MOV, zero barriers) for the CANCEL bit, then checks the
// cancel_state byte only on the cold path. Platform-specific.
bool is_cancel_pending();

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
// point. On the fast path (no cancellation — the overwhelmingly common case),
// this is a single function call whose body is one RELAXED load + branch.
// The ACQUIRE barrier from the old design is eliminated entirely.
LIBC_INLINE void check() {
  if (LIBC_LIKELY(!is_cancel_pending()))
    return;
  act();
}

// Request cancellation of the thread identified by |attrib|. Sets the
// notify::CANCEL bit on the target's notification word. For
// asynchronous cancellation, arranges prompt delivery (e.g. APC on
// Windows). For self-cancel, sets the bit and calls check() inline.
// Returns 0 on success, ESRCH if the thread is not found.
//
// Identity discipline: callers reach the lifecycle via
// `attrib->platform_data` (set when the lifecycle was allocated) — no
// thread-registry lookup is needed. The lifetime of `attrib` is the
// caller's responsibility (POSIX-trust on `pthread_t`).
int request(::LIBC_NAMESPACE::ThreadAttributes *attrib);

// Cleanup stack management. Platform-specific implementations maintain
// the per-thread cleanup handler chain (linked list of __pthread_cleanup_t).
// Uses the global-namespace type — must match ThreadLifecycle::cancel_cleanup_stack.
::__pthread_cleanup_t *get_cleanup_head();
void set_cleanup_head(::__pthread_cleanup_t *frame);

} // namespace cancel
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_CANCEL_SUPPORT_H
