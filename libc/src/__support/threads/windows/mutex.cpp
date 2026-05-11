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
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace robust_mutex {

// ---------------------------------------------------------------------------
// Process-wide robust record pool
// ---------------------------------------------------------------------------

static internal::SlabPool robust_pool;

void robust_pool_init() {
  robust_pool.init(sizeof(RobustRecord), alignof(RobustRecord));
  // Pool-managed TLS so the slab abandons via FLS at thread exit and the
  // sealed-with-live-slots Crystalline retire path can run. Without
  // this hook the per-thread slab sits forever owned by the dead TID.
  robust_pool.init_tls(internal::kTlsCleanupPhaseAllocator);
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
  lc->tid = NtCurrentThreadId();
  set_current_lifecycle(lc);
  if (!registry_register_self(lc)) {
    set_current_lifecycle(nullptr);
    free_lifecycle(lc);
    return nullptr;
  }
  return lc;
}

RobustRecord *alloc_record() {
  void *slot = robust_pool.tls_alloc();
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
  // task_id is monotonic and never recycled in-process — a hash hit
  // is always the original owner. Crystalline pins the resolved
  // lifecycle and its borrowed handle for the call frame.
  ThreadLifecycle *found =
      registry_find_by_task_id(static_cast<uint32_t>(owner_task_id));
  if (found) {
    HANDLE h = registry_borrow_handle(found);
    if (!h)
      return true; // no handle = dead
    static constexpr LARGE_INTEGER zero_timeout = {{0, 0}};
    NTSTATUS status = ::NtWaitForSingleObject(
        h, 0, const_cast<LARGE_INTEGER *>(&zero_timeout));
    return status == 0; // signaled = thread exited = dead
  }

  // Owner never registered (libc-foreign thread that attached only the
  // robust-mutex sidecar without taking the registry path), or the
  // owner exited between acquire and now. There is no kernel-side
  // probe by `task_id` — without a registered lifecycle there is no
  // handle to wait on, so treat as dead. The "raw TID" interpretation
  // would be unsafe: NT TIDs recycle and a probe could hit a different
  // thread.
  return true;
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
  // Pool-managed TLS: SlabPool::fork_reinit re-claims the surviving
  // thread's slab under the new TID. The TEB TLS slot is preserved across
  // fork (CoW); no explicit clear needed.
  LIBC_NAMESPACE::robust_mutex::robust_pool.fork_reinit();
}

namespace LIBC_NAMESPACE_DECL {
namespace internal {
// Pool VA is image-owned and reclaimed on DLL unmap; only the TEB TLS slot
// the pool reserved in init_tls() needs explicit release.
static void robust_pool_fini() {
  robust_mutex::robust_pool.fini_tls();
}
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(4, robust_pool,
                   &::LIBC_NAMESPACE::internal::robust_pool_fini)

LIBC_REGISTER_FORK_REINIT(robust_pool,
                          ::LIBC_NAMESPACE::internal::kForkPrioRobustPool,
                          &::LIBC_NAMESPACE::internal::robust_pool_fork_reinit)
