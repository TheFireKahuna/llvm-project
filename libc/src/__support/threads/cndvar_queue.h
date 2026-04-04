//===-- Intrusive wait-queue for CndVar -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared intrusive singly-linked FIFO queue used by both Linux and Windows
// CndVar implementations. Signal delivery is platform-specific — see
// linux/CndVar.cpp and windows/CndVar.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_CNDVAR_QUEUE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_CNDVAR_QUEUE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/futex.h" // Futex

namespace LIBC_NAMESPACE_DECL {

enum CndWaiterStatus : uint32_t {
  WS_Waiting = 0xE,
  WS_Signalled = 0x5,
  WS_TimedOut = 0xD,
};

struct CndWaiter {
  Futex futex_word = WS_Waiting;
  CndWaiter *next = nullptr;
};

/// Intrusive singly-linked FIFO queue of CndWaiter nodes.
/// All methods must be called with external synchronization (the CndVar qmtx).
class CndWaiterQueue {
  CndWaiter *front = nullptr;
  CndWaiter *back = nullptr;

public:
  /// Append a waiter to the back of the queue.
  /// Returns the previous back pointer (needed for rollback on failure).
  LIBC_INLINE CndWaiter *enqueue(CndWaiter *w) {
    CndWaiter *old_back = back;
    if (front == nullptr) {
      front = back = w;
    } else {
      back->next = w;
      back = w;
    }
    return old_back;
  }

  /// Remove and return the front waiter. Returns nullptr if empty.
  LIBC_INLINE CndWaiter *dequeue() {
    if (front == nullptr)
      return nullptr;
    CndWaiter *w = front;
    front = w->next;
    if (front == nullptr)
      back = nullptr;
    return w;
  }

  /// Undo the most recent enqueue. \p old_back is the value returned by
  /// enqueue(). Used when the mutex unlock fails after enqueue.
  LIBC_INLINE void rollback(CndWaiter *, CndWaiter *old_back) {
    back = old_back;
    if (back == nullptr)
      front = nullptr;
    else
      back->next = nullptr;
  }

  /// Remove a specific waiter from the queue (used for timeout cleanup).
  /// Safe no-op if the waiter has already been dequeued by a signaller.
  LIBC_INLINE void remove(CndWaiter *waiter) {
    if (front == waiter) {
      front = waiter->next;
      if (front == nullptr)
        back = nullptr;
      return;
    }
    for (CndWaiter *prev = front; prev != nullptr; prev = prev->next) {
      if (prev->next == waiter) {
        prev->next = waiter->next;
        if (back == waiter)
          back = prev;
        return;
      }
    }
  }

  /// Detach all waiters from the queue. Returns the head of the chain.
  LIBC_INLINE CndWaiter *detach_all() {
    CndWaiter *head = front;
    front = back = nullptr;
    return head;
  }

  LIBC_INLINE bool empty() const { return front == nullptr; }
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_CNDVAR_QUEUE_H
