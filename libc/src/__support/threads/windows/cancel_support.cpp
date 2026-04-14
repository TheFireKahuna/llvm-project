//===-- Windows cancel support implementation -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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
  return &lc->cancel_word;
}

// APC callback for asynchronous cancellation. Fired as a special user APC,
// so it runs without requiring an alertable wait.
static void NTAPI cancel_apc(PVOID, PVOID, PVOID) { check(); }

int request(int tid) {
  DWORD dtid = static_cast<DWORD>(tid);

  // Self-cancel: set pending bit and check immediately.
  if (dtid == ::NtCurrentThreadId()) {
    auto *word = get_cancel_word();
    if (!word)
      return ESRCH;
    word->fetch_or(PENDING_BIT, cpp::MemoryOrder::RELEASE);
    check();
    // check() returns only if cancellation is disabled.
    return 0;
  }

  {
    EpochGuard guard;
    auto *target = guard.find(dtid);
    if (!target)
      return ESRCH;

    uint8_t prev =
        target->cancel_word.fetch_or(PENDING_BIT, cpp::MemoryOrder::RELEASE);

    // If asynchronous cancellation is enabled, queue a special user APC to
    // interrupt the target immediately.
    if (prev & TYPE_BIT) {
      HANDLE h = registry_borrow_handle(target);
      if (h)
        windows::queue_special_user_apc(h, cancel_apc);
    }
  }

  // Wake the target from any blocking wait (futex, sleep, etc.) so it
  // reaches the next cancellation point promptly. No epoch pin needed —
  // alert uses a TID (value type), not a borrowed pointer.
  ::NtAlertThreadByThreadId(
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(dtid)));

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
