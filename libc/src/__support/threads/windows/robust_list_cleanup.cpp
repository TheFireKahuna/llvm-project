//===-- Robust mutex cleanup for dying threads --------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/robust_list_cleanup.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/mutex.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

namespace LIBC_NAMESPACE_DECL {
namespace robust_mutex {

// Futex word encoding (must match unix_mutex.h).
static constexpr FutexValueType ROBUST_OWNER_DIED = 0x80000000u;

void robust_list_cleanup(void *lifecycle_ptr) {
  auto *lc = static_cast<ThreadLifecycle *>(lifecycle_ptr);
  if (!lc)
    return;

  RobustRecord *record = lc->robust_list;
  while (record) {
    RobustRecord *next = record->next;
    if (record->futex_word) {
      // Single-instruction atomic OR — sets OWNER_DIED while preserving
      // all existing bits (including WAITERS). No CAS loop needed.
      record->futex_word->fetch_or(ROBUST_OWNER_DIED,
                                   cpp::MemoryOrder::RELEASE);
      // Wake all waiters so they can observe OWNER_DIED.
      record->futex_word->notify_all();
    }
    // Return the record to the process-wide robust_pool.
    internal::SlabPool::free(record);
    record = next;
  }

  lc->robust_list = nullptr;
}

} // namespace robust_mutex
} // namespace LIBC_NAMESPACE_DECL
