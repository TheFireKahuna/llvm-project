//===-- Windows cancel support implementation -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cancellation support using the ThreadLocalWord notification word.
//
// The cancel-pending notification lives in notify_word (notify::CANCEL bit),
// not in cancel_state. This separates the cross-thread notification channel
// (notify_word — atomic signal_or + conditional kernel alert) from the
// owner-only state bits (cancel_state — plain reads, zero barriers).
//
// Hot path (cancel::check → is_cancel_pending):
//   1. get_current_lifecycle()      — single TEB inline slot read (one MOV)
//   2. lc->notify_word.read()       — RELAXED load (plain MOV, no barrier)
//   3. test notify::CANCEL bit      — AND + branch (fast exit if clear)
//   4. lc->cancel_state RELAXED     — only on cold path (cancel IS pending)
//
// Old design cost: 3-hop thread_local read + ACQUIRE load (ldar on AArch64).
// New design cost: 1 TEB MOV + 1 plain MOV. ~3x fewer instructions, zero
// barriers on any architecture.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/cancel_support.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/apc.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/OSUtil/windows/signal/syscall_frame.h"

namespace LIBC_NAMESPACE_DECL {
namespace cancel {

cpp::Atomic<uint8_t> *get_cancel_word() {
  auto *lc = get_current_lifecycle();
  if (!lc)
    return nullptr;
  return &lc->cancel_state;
}

bool is_cancel_pending() {
  auto *lc = get_current_lifecycle();
  if (LIBC_LIKELY(!lc))
    return false;
  // Fast path: single RELAXED load of notify_word — plain MOV on x86,
  // plain LDR on AArch64. No barrier on any architecture.
  if (LIBC_LIKELY(!(lc->notify_word.read() & notify::CANCEL)))
    return false;
  // Cold path: cancel IS pending — check if cancellation is enabled.
  // cancel_state is owner-only; RELAXED is correct (same-thread sequencing).
  return !(lc->cancel_state.load(cpp::MemoryOrder::RELAXED) & STATE_BIT);
}

// APC callback for asynchronous cancellation. Fired as a special user APC,
// so it runs without requiring an alertable wait.
NTAPI static void cancel_apc(PVOID, PVOID, PVOID) { check(); }

int request(ThreadAttributes *attrib) {
  if (!attrib)
    return ESRCH;
  auto *target = static_cast<ThreadLifecycle *>(attrib->platform_data);
  if (!target)
    return ESRCH;

  DWORD target_tid = target->tid;

  // Self-cancel: set notify::CANCEL on our own notify_word and check.
  // Owner-thread write — no atomic RMW needed. read() + write() are both
  // RELAXED (plain MOV), safe because no concurrent writer for self.
  if (target_tid == ::NtCurrentThreadId()) {
    target->notify_word.write(target->notify_word.read() | notify::CANCEL);
    check();
    // check() returns only if cancellation is disabled.
    return 0;
  }

  // Cross-thread cancel via pthread_t bypass — POSIX-trust applies on
  // attrib (UB to use after pthread_join/detach). No registry lookup.

  // Set notify::CANCEL on the target's notification word.
  // signal_or: atomic OR + generation bump + conditional kernel alert
  // (fires NtAlertThreadByThreadId if the target is parked in
  // ThreadLocalWord::wait_for_change).
  ThreadLocalWord::signal_or(&target->notify_word, notify::CANCEL);

  // If asynchronous cancellation is enabled, queue a special user APC
  // to interrupt the target immediately — even if it is busy-spinning
  // and not at a cooperative dispatch boundary.
  uint8_t cs = target->cancel_state.load(cpp::MemoryOrder::RELAXED);
  if (cs & TYPE_BIT) {
    HANDLE h = registry_borrow_handle(target);
    if (h)
      windows::queue_special_user_apc(h, cancel_apc);
  }

  // Wake the target from any blocking wait (futex, IoRing, sleep, etc.)
  // so it reaches the next cancellation point promptly. signal_or's
  // conditional alert only fires when the target is in
  // ThreadLocalWord::wait_for_change(); this unconditional alert covers
  // all other NT wait mechanisms. Idempotent — harmless if signal_or
  // already fired the alert.
  ::NtAlertThreadByThreadId(
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(target_tid)));

  return 0;
}

void before_unwind() {
  // Cancel any in-flight I/O before forced unwind destroys stack frames.
  // SyscallFrame lives on the stack — once the unwinder walks past it,
  // the pointer is stale. Walk the chain and cancel everything now.
  auto *lc = get_current_lifecycle();
  if (!lc)
    return;
  auto *frame = lc->active_syscall.load(cpp::MemoryOrder::ACQUIRE);
  if (frame) {
    signal_state::cancel_inflight_io(frame);
    lc->active_syscall.store(nullptr, cpp::MemoryOrder::RELEASE);
  }
}

::__pthread_cleanup_t *get_cleanup_head() {
  auto *lc = get_current_lifecycle();
  if (!lc)
    return nullptr;
  return lc->cancel_cleanup_stack;
}

void set_cleanup_head(::__pthread_cleanup_t *frame) {
  auto *lc = get_current_lifecycle();
  if (lc)
    lc->cancel_cleanup_stack = frame;
}

} // namespace cancel
} // namespace LIBC_NAMESPACE_DECL
