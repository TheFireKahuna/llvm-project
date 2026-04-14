//===--- Lock-free wait slot pool for Treiber futex -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each waiting thread needs a slot to publish its thread ID for the waker.
// Slots are pooled globally, one per thread, allocated via TLS on first use.
// The pool itself is a static array — zero dynamic allocation.
//
// The freelist is a Treiber stack of slot indices, using the same lock-free
// CAS pattern as the futex wait queue. Slot 0 is the null sentinel.
//
// Nesting: VEH handlers can fire during user-mode phases of Futex::wait
// (spin, CAS push). If the thread's primary slot is already linked,
// alloc_secondary() provides a fresh slot from the freelist. At most one
// level of nesting occurs (hardware exception during futex wait).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_WAIT_SLOT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_WAIT_SLOT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace wait_slot {

// Slot states:
//   IDLE → WAITING → IN_KERNEL → SIGNALED → IDLE   (normal wake)
//                  ↘ SIGNALED → IDLE                 (fast wake before kernel entry)
//                               ↘ TIMED_OUT          (timeout, reclaimed by waker)
//
// After spurious wake from NtWaitForAlertByThreadId, the waiter loads
// state: IN_KERNEL means no waker raced (safe to re-sleep), SIGNALED
// means a waker claimed us. This works because the waker can only do
// IN_KERNEL → SIGNALED — never IN_KERNEL → anything else.
enum SlotState : uint8_t {
  IDLE = 0,
  WAITING = 1,   // Pushed onto Treiber stack, not yet in kernel.
  IN_KERNEL = 2, // Blocked in NtWaitForAlertByThreadId.
  SIGNALED = 3,  // Claimed by waker — waiter should return.
  TIMED_OUT = 4, // Timeout or thread exit — dead entry.
};

struct alignas(64) WaitSlot {
  cpp::Atomic<uint8_t> state{IDLE};
  uint8_t handoff{0}; // Set by waker in handoff_one, read by waiter on SIGNALED.
  uint32_t thread_id{0};
  uint32_t next{0}; // SLL forward link / freelist link (pool index, 0 = tail)
  // Bumped on alloc and free — detects stale TLS refs. Atomic because
  // the owning thread reads it in get_slot_index while a waker may
  // concurrently increment it in free_slot (via reclaim_slot).
  cpp::Atomic<uint16_t> generation{0};
  // Target address for waits (both embedded Futex and address-keyed).
  // Used by the parking lot to match waiters to the correct futex.
  uintptr_t wait_address{0};
};

// Index 0 is the null sentinel.
inline constexpr uint32_t NULL_INDEX = 0;

// The pool and freelist are in an anonymous namespace in the .cpp to avoid
// multiple-definition issues. These functions provide the interface.

// Get the calling thread's slot index (TLS fast path). Allocates on first use.
uint32_t get_slot_index();

// Access a slot by index.
WaitSlot &get_slot(uint32_t index);

// Allocate a secondary slot for nested waits (VEH during futex wait).
// Returns NULL_INDEX if pool exhausted. Caller must release_secondary().
uint32_t alloc_secondary();

// Return a secondary slot to the freelist after a nested wait completes.
void release_secondary(uint32_t index);

// Return a slot to the freelist (called by TLS cleanup on thread exit).
void free_slot(uint32_t index);

// Reclaim a dead slot popped from a futex chain by a waker.
// The slot must already be popped (not reachable from any chain).
void reclaim_slot(uint32_t index);

// Check if the calling thread's TLS slot is TIMED_OUT (still linked in a
// bucket DLL from a previous wait). Returns the slot index and sets
// wait_address_out to the slot's wait_address. Returns NULL_INDEX if the
// thread has no stale slot. Used by the futex wait path for deferred
// reclamation — the timeout path is lock-free, and the slot is cleaned up
// on the next wait by the same thread.
uint32_t get_stale_slot(uintptr_t &wait_address_out);

// Initialize the wait slot subsystem (TLS allocation, freelist setup).
// Called once from startup.
void init();

// Tear down (TLS free). Called from shutdown.
void fini();

// Reset the wait slot pool after fork (child process).
void fork_reinit();

} // namespace wait_slot
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_WAIT_SLOT_H
