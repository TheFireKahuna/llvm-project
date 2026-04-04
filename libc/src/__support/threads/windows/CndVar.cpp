//===-- Windows CndVar implementation ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Same intrusive-queue algorithm as the Linux implementation. The only
// difference is signal delivery: Linux uses FUTEX_WAKE_OP to atomically
// set the waiter's futex word and wake it in a single syscall. Windows
// has no equivalent, so we use a CAS + WakeByAddressSingle pair. The
// CAS on the waiter's futex_word is the sole arbitrator between signal
// and timeout — exactly one wins, no race.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/CndVar.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/mutex.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"     // Mutex
#include "src/__support/threads/raw_mutex.h" // RawMutex

namespace LIBC_NAMESPACE_DECL {

int CndVar::wait(Mutex *m) { return wait(m, cpp::nullopt); }

int CndVar::wait(Mutex *m, cpp::optional<Futex::Timeout> timeout) {
  CndWaiter waiter;
  {
    cpp::lock_guard ml(qmtx);
    CndWaiter *old_back = waitq.enqueue(&waiter);

    if (m->unlock() != MutexError::NONE) {
      waitq.rollback(&waiter, old_back);
      return -1;
    }
  }

  // Wait loop — re-waits on spurious futex wakes until signalled or timed out.
  //
  // Fatal-error exit: any negative return other than -EINTR (and -ETIMEDOUT
  // which is handled below) means the wait cannot make progress — e.g.
  // -ENOMEM from wait-slot pool exhaustion or -EINVAL from mutex invalidation.
  // Looping would spin-fail identically. Treat as "timed out" so the
  // arbitration CAS below still runs and the waiter is removed from the
  // queue cleanly; callers see ETIMEDOUT rather than a silent hang.
  // The default Futex::wait<false> path absorbs APC-only wakes internally,
  // so -EINTR never surfaces here from the interruptible=true call above
  // unless the caller explicitly enabled cancellation.
  bool timed_out = false;
  while (waiter.futex_word.load(cpp::MemoryOrder::ACQUIRE) == WS_Waiting) {
    int ret = waiter.futex_word.wait(WS_Waiting, timeout, true);
    if (ret == -ETIMEDOUT) {
      timed_out = true;
      break;
    }
    if (ret < 0 && ret != -EINTR) {
      timed_out = true;
      break;
    }
  }

  if (!timed_out) {
    // Signalled — notify_one/broadcast already dequeued us and set
    // WS_Signalled. Re-lock and return success.
    auto err = m->lock();
    return err == MutexError::NONE ? 0 : -1;
  }

  // Timed out — arbitrate with CAS. Exactly one of {timeout, signal} wins.
  FutexValueType expected = WS_Waiting;
  if (waiter.futex_word.compare_exchange_strong(
          expected, WS_TimedOut, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::ACQUIRE)) {
    // Timeout won. Remove from queue (may be no-op if signaller already
    // popped us — the CAS guarantees the signaller will see WS_TimedOut
    // and skip us, but it may have already dequeued the node).
    {
      cpp::lock_guard ml(qmtx);
      waitq.remove(&waiter);
    }
    auto err = m->lock();
    return err == MutexError::NONE ? ETIMEDOUT : -1;
  }

  // Signal won the race — already dequeued and signalled.
  auto err = m->lock();
  return err == MutexError::NONE ? 0 : -1;
}

void CndVar::notify_one() {
  cpp::lock_guard ml(qmtx);
  while (CndWaiter *first = waitq.dequeue()) {
    // CAS arbitration: claim this waiter.
    FutexValueType expected = WS_Waiting;
    if (first->futex_word.compare_exchange_strong(
            expected, WS_Signalled, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::RELAXED)) {
      first->futex_word.notify_one(/*is_shared=*/true);
      return;
    }
    // Waiter timed out (WS_TimedOut) — skip, try next.
  }
}

void CndVar::broadcast() {
  cpp::lock_guard ml(qmtx);
  CndWaiter *waiter = waitq.detach_all();

  while (waiter != nullptr) {
    CndWaiter *next = waiter->next;
    FutexValueType expected = WS_Waiting;
    if (waiter->futex_word.compare_exchange_strong(
            expected, WS_Signalled, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::RELAXED)) {
      waiter->futex_word.notify_one(/*is_shared=*/true);
    }
    // If CAS failed (WS_TimedOut), skip — waiter is self-removing.
    waiter = next;
  }
}

} // namespace LIBC_NAMESPACE_DECL
