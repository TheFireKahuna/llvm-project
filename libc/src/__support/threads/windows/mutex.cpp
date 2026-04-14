//===-- Windows mutex support ------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/mutex.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace robust_mutex {

// ---------------------------------------------------------------------------
// Process-wide robust record pool
// ---------------------------------------------------------------------------

static internal::SlabPool robust_pool;
static thread_local internal::SlabPool::ThreadSlab robust_current_slab{nullptr};

void robust_pool_init() {
  robust_pool.init(sizeof(RobustRecord), alignof(RobustRecord));
}

namespace {

ThreadLifecycle *ensure_current_lifecycle() {
  auto *lc = get_current_lifecycle();
  if (lc)
    return lc;

  lc = alloc_lifecycle();
  if (!lc)
    return nullptr;
  lc->task_id = allocate_task_id();
  lc->tid = static_cast<int>(NtCurrentThreadId());
  lc->pool_allocated = true;
  set_current_lifecycle(lc);
  registry_register_self(lc);
  return lc;
}

RobustRecord *alloc_record() {
  void *slot = internal::SlabPool::alloc(robust_current_slab);
  if (!slot)
    slot = robust_pool.alloc_slow(&robust_current_slab);
  if (!slot)
    return nullptr;
  auto *record = static_cast<RobustRecord *>(slot);
  record->next = nullptr;
  record->prev_next = nullptr;
  record->futex_word = nullptr;
  return record;
}

void free_record(RobustRecord *record) {
  if (!record)
    return;
  internal::SlabPool::free(record);
}

} // namespace

FutexValueType get_robust_owner_id() {
  auto *lc = ensure_current_lifecycle();
  if (lc)
    return static_cast<FutexValueType>(lc->task_id);
  // Fallback for foreign threads that haven't attached yet.
  return static_cast<FutexValueType>(NtCurrentThreadId());
}

bool is_owner_dead(FutexValueType owner_task_id) {
  {
    EpochGuard guard;
    ThreadLifecycle *found =
        guard.find_by_task_id(static_cast<uint32_t>(owner_task_id));
    if (found) {
      HANDLE h = registry_borrow_handle(found);
      if (!h)
        return true; // no handle = dead
      static constexpr LARGE_INTEGER zero_timeout = {{0, 0}};
      NTSTATUS status = ::NtWaitForSingleObject(
          h, 0, const_cast<LARGE_INTEGER *>(&zero_timeout));
      return status == 0; // signaled = thread exited = dead
    }
  }

  // Not in registry — treat the owner id as a raw NT thread id and
  // probe it directly. This covers foreign threads that attached only
  // enough lifecycle state to use robust mutexes.
  CLIENT_ID cid = {};
  cid.UniqueThread =
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(owner_task_id));
  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  HANDLE h = nullptr;
  NTSTATUS st = ::NtOpenThread(&h, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                               &oa, &cid);
  if (!NT_SUCCESS(st))
    return true;

  static constexpr LARGE_INTEGER zero_timeout = {{0, 0}};
  NTSTATUS wait_st = ::NtWaitForSingleObject(
      h, 0, const_cast<LARGE_INTEGER *>(&zero_timeout));
  ::NtClose(h);
  return wait_st == 0;
}

void robust_list_add(Futex *futex_word, RobustRecord **record_slot) {
  if (!record_slot || *record_slot)
    return;

  auto *lc = ensure_current_lifecycle();
  RobustRecord *record = alloc_record();
  if (!record)
    return;

  record->futex_word = futex_word;
  record->next = lc->robust_list;
  record->prev_next = &lc->robust_list;
  if (record->next)
    record->next->prev_next = &record->next;
  lc->robust_list = record;
  *record_slot = record;
}

void robust_list_remove(RobustRecord **record_slot) {
  if (!record_slot || !*record_slot)
    return;

  RobustRecord *record = *record_slot;
  *record_slot = nullptr;

  if (record->prev_next)
    *record->prev_next = record->next;
  if (record->next)
    record->next->prev_next = record->prev_next;

  free_record(record);
}

void lifecycle_destroy(ThreadLifecycle *lc) {
  if (!lc)
    return;

  // Free all remaining robust records back to the pool.
  RobustRecord *record = lc->robust_list;
  while (record) {
    RobustRecord *next = record->next;
    free_record(record);
    record = next;
  }
  lc->robust_list = nullptr;
}

} // namespace robust_mutex
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::robust_pool_startup_init() {
  LIBC_NAMESPACE::robust_mutex::robust_pool_init();
  return 0;
}

void LIBC_NAMESPACE::internal::robust_pool_fork_reinit() {
  LIBC_NAMESPACE::robust_mutex::robust_current_slab = nullptr;
  LIBC_NAMESPACE::robust_mutex::robust_pool.fork_reinit();
}
