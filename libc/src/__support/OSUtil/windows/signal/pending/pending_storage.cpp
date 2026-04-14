//===-- Pending signal storage (non-inline functions) -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"

#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace signal_pending {

void clear_signal(PendingSet &ps, int signum) {
  uint64_t bit = 1ULL << (signum - 1);

  // Standard (non-RT) signals: clear the bitmap bit and return.
  if (signum < SIGRTMIN) {
    ps.standard.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
    return;
  }

  // RT signals: clear the target bit FIRST, then drain matching entries.
  //
  // This ordering is critical for producer-safety. The alternative (drain
  // first, then clear bit) has a race:
  //
  //   1. We drain inbox (all entries removed)
  //   2. Concurrent pend_rt() pushes new entry, sets bit via fetch_or
  //   3. We clear bit via CAS
  //   → Entry in inbox with bit cleared — invisible to dispatch.
  //
  // With bit-first clearing, concurrent pend_rt() re-sets its own bit via
  // fetch_or AFTER our fetch_and, so the new entry is properly tracked.
  // The worst case is a stale bit (pend_rt sets bit, we drain and free
  // its entry, bit remains set) — the dispatch engine already handles
  // stale RT bits with one extra empty drain pass.
  ps.standard.fetch_and(~bit, cpp::MemoryOrder::ACQ_REL);

  // Drain inbox: free entries matching signum, keep the rest.
  SigqueueEntry *keep_head = nullptr;
  SigqueueEntry *keep_tail = nullptr;

  while (SigqueueEntry *entry = ps.inbox.try_pop()) {
    if (entry->si_signo == signum) {
      sigqueue_free(entry);
    } else {
      entry->next.store(nullptr, cpp::MemoryOrder::RELAXED);
      if (!keep_head) {
        keep_head = entry;
      } else {
        keep_tail->next.store(entry, cpp::MemoryOrder::RELAXED);
      }
      keep_tail = entry;
    }
  }

  // Restore survivor presence bits BEFORE re-pushing entries.
  //
  // Ordering matters: if we push first and set bits after, there is a
  // window where entries exist in the inbox with no corresponding bitmap
  // bit — the dispatch engine would miss them. Setting bits first creates
  // at most a stale bit (bit set, entry not yet visible), which the
  // dispatch engine already handles with one extra empty drain pass.
  //
  // fetch_or is safe against concurrent pend_rt: both only SET bits.
  uint64_t repush_bits = 0;
  for (SigqueueEntry *scan = keep_head; scan;
       scan = scan->next.load(cpp::MemoryOrder::RELAXED))
    repush_bits |= 1ULL << (scan->si_signo - 1);

  if (repush_bits)
    ps.standard.fetch_or(repush_bits, cpp::MemoryOrder::RELEASE);

  // Now re-push survivors in FIFO order.
  SigqueueEntry *cur = keep_head;
  while (cur) {
    SigqueueEntry *next = cur->next.load(cpp::MemoryOrder::RELAXED);
    ps.inbox.push(cur);
    cur = next;
  }
}

void clear_all(PendingSet &ps) {
  // Drain and free all inbox entries first, then clear the bitmap.
  // Order matters: clearing the bitmap first would let a concurrent
  // dispatch see an empty bitmap while entries still exist in the inbox.
  while (SigqueueEntry *entry = ps.inbox.try_pop())
    sigqueue_free(entry);

  ps.standard.store(0, cpp::MemoryOrder::RELEASE);
}

} // namespace signal_pending
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
