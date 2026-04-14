//===-- Vyukov MPSC queue for RT signal entries -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lock-free intrusive MPSC FIFO queue for real-time signal entries.
//
// Based on Dmitry Vyukov's intrusive MPSC queue (2010). Used by the Linux
// kernel (task_work), Tokio, and Crossbeam. Properties:
//
//   Enqueue: 1 XCHG (wait-free)
//   Dequeue: 1 load  (wait-free, single consumer only)
//   FIFO:    by construction — no reversal, no reordering
//   ABA:     impossible — no CAS on dequeue path
//
// One queue instance exists per RT signal number per PendingSet. This
// satisfies POSIX lowest-numbered-first delivery across different RT signals
// while preserving FIFO within the same signal number.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_RT_QUEUE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_RT_QUEUE_H

#include "hdr/signal_macros.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/uid_t.h"
#include "hdr/types/union_sigval.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// SigqueueEntry — intrusive node + signal payload
// ---------------------------------------------------------------------------
//
// Pool-allocated via SlabPool (sigqueue_pool). The `next` field is atomic
// because producers write it from arbitrary threads/contexts (APC, VEH).
// The payload fields are written once by the producer and read once by
// the consumer — no atomics needed for them.

struct SigqueueEntry {
  cpp::Atomic<SigqueueEntry *> next; // Queue link (atomic for push)
  int si_signo;                      // Signal number
  int si_code;                       // SI_USER, SI_QUEUE, SI_TIMER, etc.
  union sigval value;                // Payload (pointer-sized)
  pid_t pid;                         // Sender PID
  uid_t uid;                         // Sender UID (POSIX requires for sigqueue)
};

// 8 (atomic ptr) + 4 + 4 + 8 + 4 + 4 = 32, fits in one SlabPool slot.
static_assert(sizeof(SigqueueEntry) <= 32,
              "SigqueueEntry exceeds 32 bytes — review SlabPool slot size");

// ---------------------------------------------------------------------------
// RtQueue — Vyukov MPSC FIFO
// ---------------------------------------------------------------------------
//
// Thread safety:
//   push()    — safe to call from any thread, any context (APC, VEH, normal).
//   try_pop() — single consumer only (the owning thread's drain loop).
//   empty()   — may be called from any thread (conservative check).
//
// The queue uses an embedded stub node to simplify the empty-state logic.
// The stub is never returned to callers — try_pop() transparently skips it.

class RtQueue {
  SigqueueEntry stub_;                // Sentinel node, never dequeued
  SigqueueEntry *head_;               // Consumer-private, no atomic needed
  cpp::Atomic<SigqueueEntry *> tail_; // Producers exchange on this

public:
  // Initialize to empty state. Must be called before first use.
  // Not a constructor — PendingSet is zero-initialized, then init'd explicitly.
  LIBC_INLINE void init() {
    stub_.next.store(nullptr, cpp::MemoryOrder::RELAXED);
    head_ = &stub_;
    tail_.store(&stub_, cpp::MemoryOrder::RELAXED);
  }

  // Producer: lock-free, wait-free. One XCHG + one store.
  //
  // Safe to call from APC context, VEH context, signal handler context,
  // or normal execution. Multiple producers are serialized by the XCHG
  // on tail_ — each gets a unique predecessor to link to.
  //
  // Memory ordering:
  //   - node->next store is RELEASE: publishes the node's payload fields
  //     (si_signo, si_code, value, etc.) written before push().
  //   - tail_ exchange is ACQ_REL: acquires the predecessor's state (so we
  //     can safely store into prev->next), releases our node to the consumer.
  LIBC_INLINE void push(SigqueueEntry *node) {
    node->next.store(nullptr, cpp::MemoryOrder::RELEASE);
    SigqueueEntry *prev = tail_.exchange(node, cpp::MemoryOrder::ACQ_REL);
    prev->next.store(node, cpp::MemoryOrder::RELEASE);
  }

  // Consumer: wait-free. Single-thread only (owning thread's drain loop).
  //
  // Returns nullptr if empty or if the queue is in a brief "incompleteness
  // window" — a producer has done the XCHG on tail_ but hasn't yet written
  // prev->next. This window is 1-2 instructions wide. For signals, returning
  // nullptr and picking up the entry on the next drain pass (microseconds
  // later at the next dispatch boundary) is correct.
  //
  // The caller MUST NOT free the stub node. The returned pointer is always
  // a real SigqueueEntry allocated from sigqueue_pool.
  LIBC_INLINE SigqueueEntry *try_pop() {
    SigqueueEntry *h = head_;
    SigqueueEntry *next = h->next.load(cpp::MemoryOrder::ACQUIRE);

    // Skip the stub node transparently.
    if (h == &stub_) {
      if (!next)
        return nullptr; // Empty queue
      head_ = next;     // Advance past stub
      h = next;
      next = h->next.load(cpp::MemoryOrder::ACQUIRE);
    }

    // Fast path: h has a successor — dequeue h.
    if (next) {
      head_ = next;
      return h;
    }

    // h is the last node. Check if a producer is mid-push.
    if (tail_.load(cpp::MemoryOrder::ACQUIRE) != h)
      return nullptr; // Incompleteness window — retry on next drain pass

    // h is truly the last node. Re-insert stub as new tail so we can
    // dequeue h without losing our sentinel.
    push(&stub_);
    next = h->next.load(cpp::MemoryOrder::ACQUIRE);
    if (next) {
      head_ = next;
      return h;
    }

    // Stub push is in the incompleteness window. h will be dequeued on
    // the next drain pass.
    return nullptr;
  }

  // Conservative emptiness check. Must be called from the consumer thread
  // (same thread that calls try_pop) — head_ is consumer-private.
  //
  // Returns true if the queue appears empty. May return false negatives
  // (queue has entries but the link isn't visible yet) — never false
  // positives. Used for fast-path skip in dispatch: if empty() returns
  // true, there's no point entering the drain loop for this signal.
  LIBC_INLINE bool empty() const {
    // If head is the stub: queue is empty iff stub has no successor.
    // If head is not the stub: queue definitely has entries.
    const SigqueueEntry *h = head_;
    if (h == &stub_)
      return const_cast<cpp::Atomic<SigqueueEntry *> &>(h->next)
                 .load(cpp::MemoryOrder::ACQUIRE) == nullptr;
    return false;
  }

  // Check if the queue has been initialized. Detects zero-initialized state.
  LIBC_INLINE bool is_initialized() const {
    return head_ != nullptr;
  }
};

// RtQueue: stub_ (32) + head_ (8) + tail_ (8) = 48 bytes.
static_assert(sizeof(RtQueue) <= 48,
              "RtQueue grew — review PendingSet layout");

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PENDING_RT_QUEUE_H
