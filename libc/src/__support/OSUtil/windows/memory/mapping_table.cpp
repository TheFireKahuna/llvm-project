//===-- Mapping-table bring-up: .libcmem handler + fork / fini ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tier A Phase 0c.5 runs the unified `.libcmem` walker (see
// memory_primitives_bootstrap.{h,cpp}). The walker sweeps every handler
// registered into `.libcmem$M`; this TU contributes the mapping-table
// handler:
//
//   1. Pass 1 `mapping_table_init_fn` calls
//      `g_mapping_table.ensure_init()` — reserves L1, the remap-guard
//      region, and the RegionPool sentinels. Emits zero Receipts: the
//      table's own radix-page backing is internal bookkeeping, not a
//      valid mmap / register_mapping target.
//   2. Pass 2 (walker-driven) stamps every Receipt from every handler
//      (substrate seeds, ...) as LIBC_INTERNAL.
//   3. Post-pass (walker-driven) calls `va_inventory_startup_discover`
//      to classify pre-existing VA as KERNEL / IMAGE / FOREIGN and
//      install the DLL load/unload callback.
//
// L1 / L2 / L3 radix pages are carved out of the VA substrate
// (alloc::VaSubstrate) — L1 / L2 from class Small (16 KB slots),
// L3 from class Large (128 KB slots). Substrate seed arenas are
// stamped LIBC_INTERNAL by the walker's Pass 2; every later
// substrate arena auto-registers itself via g_mapping_table_ready,
// so post-bootstrap L2 / L3 growth is always covered.
//
// Also handles fork reinit + shutdown teardown via the existing
// .libcfin-registered `mapping_table_fini`.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/mapping_table.h"

#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

// The sole definition of g_mapping_table. Every consumer reaches the
// table through this symbol; the extern declaration in mapping_table.h
// makes it the static-archive pull anchor for this TU's `.libcmem$P2`
// registry entry. See the comment at that declaration for rationale.
namespace LIBC_NAMESPACE_DECL {
namespace windows {
MappingTable g_mapping_table;
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Latch for "the mapping table accepts inline `register_mapping_internal`
// from any thread without deadlocking on a same-thread reentrant
// `ensure_init`." Defined alongside mapping_table.cpp's other internals so
// the references in `mapping_table_finalize_init()` resolve from the same
// TU (hermetic test exes that pull mapping_table.cpp.obj without the rest
// of the bootstrap link closure rely on this co-location).
cpp::Atomic<bool> g_mapping_table_ready{false};

// `.libcmem` handler. Brings up the mapping table + RegionPool, then
// harvests every pending internal-region receipt that queued up while
// the table was still INIT_IN_PROGRESS. The walker's Pass 2 stamps
// every receipt as LIBC_INTERNAL.
//
// Two contributors enqueue:
//   - ThreadScratch's bootstrap-tier path (transient LDR workers).
//   - Substrate's `reserve_new_arena`, when called pre-INIT_READY (e.g.
//     by `ensure_init` itself if the Small seed arena drains during
//     mapping-table bring-up).
//
// Ordering rationale:
//
//   1. `ensure_init()` drives the table to INIT_READY. The substrate
//      reserves it triggers internally land in the pending queue (the
//      `is_mapping_table_ready()` gate is still false until Pass 2
//      finishes).
//
//   2. `harvest_pending_internal_receipts` takes the same spinlock that
//      every enqueue path takes, draining the queue into the walker's
//      `out` buffer. Holding the lock across harvest closes the multi-
//      thread race where an enqueuer could observe
//      `is_init_ready() == false` and enqueue between the `ensure_init`
//      return and the harvest.
//
//   3. Walker Pass 2 stamps each receipt; walker latches
//      `g_mapping_table_ready` only after Pass 2 completes.
//
// A non-zero return here advertises pending receipts to the walker;
// the walker pre-validates `produced <= cap_remaining`. The pending
// queue's cap fits beneath kMemPrimitiveReceiptCap (32) with substantial
// headroom.
static uint32_t mapping_table_init_fn(Receipt *out, uint32_t cap) {
  if (LIBC_UNLIKELY(
          !LIBC_NAMESPACE::windows::g_mapping_table.ensure_init()))
    __builtin_trap();
  return ::LIBC_NAMESPACE::internal::scratch_detail::
      harvest_pending_internal_receipts(out, cap);
}

// Atomically latch `g_mapping_table_ready = true` and stamp every
// receipt that queued up between `mapping_table_init_fn`'s harvest and
// the end of the walker's Pass 2.
//
// Why this is structurally needed: Pass 2's `register_mapping_internal`
// loop can transitively call substrate's `slow_acquire_arena` (via
// `ensure_slot` allocating fresh L3 pages from the substrate). If that
// acquire's pool runs out of headroom and triggers a fresh
// `reserve_new_arena`, the new arena enqueues into the pending-receipts
// queue. Without this finalize, those late-comers would sit in the
// queue with `g_mapping_table_ready` already latched — orphaned.
//
// The ready-latch is performed under the pending-receipts lock so that
// any enqueuer that takes the lock afterwards observes ready=true and
// falls through to the inline-stamp path. Drained receipts are stamped
// AFTER the lock release so register_mapping_internal cannot deadlock
// against a recursive substrate enqueue (those recursive enqueuers see
// ready=true and inline-stamp themselves).
void mapping_table_finalize_init() {
  using ::LIBC_NAMESPACE::internal::scratch_detail::
      drain_pending_internal_receipts_locked;
  using ::LIBC_NAMESPACE::internal::scratch_detail::
      pending_internal_receipts_lock;
  using ::LIBC_NAMESPACE::internal::scratch_detail::
      pending_internal_receipts_unlock;

  Receipt buf[::LIBC_NAMESPACE::internal::scratch_detail::
                  kPendingInternalReceiptsCap];
  uint32_t n;

  pending_internal_receipts_lock();
  // Latch ready=true under the lock so any waiter that acquires the
  // lock after this point sees ready and inline-stamps.
  mark_mapping_table_ready();
  n = drain_pending_internal_receipts_locked(
      buf, ::LIBC_NAMESPACE::internal::scratch_detail::
               kPendingInternalReceiptsCap);
  pending_internal_receipts_unlock();

  for (uint32_t i = 0; i < n; ++i) {
    if (LIBC_UNLIKELY(
            !LIBC_NAMESPACE::windows::g_mapping_table
                 .register_mapping_internal(buf[i].base, buf[i].size)))
      __builtin_trap();
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Phase 2 — runs after va_substrate. Depends on Zone 0 substrate_secret
// being seeded (phase 1 seeds it) and on the substrate seed arenas
// already existing as receipts in the walker's stack buffer; Pass 2 of
// the walker stamps them as LIBC_INTERNAL through the table this
// handler brings up.
LIBC_REGISTER_MEMORY_PRIMITIVE(
    mapping_table, 2, &::LIBC_NAMESPACE::internal::mapping_table_init_fn)

void LIBC_NAMESPACE::internal::mapping_table_fork_reinit() {
  LIBC_NAMESPACE::windows::g_mapping_table.fork_reinit();
}

namespace LIBC_NAMESPACE_DECL {
namespace internal {
static void mapping_table_fini() {
  LIBC_NAMESPACE::windows::g_mapping_table.destroy();
}
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(4, mapping_table,
                   &LIBC_NAMESPACE::internal::mapping_table_fini)

LIBC_REGISTER_FORK_REINIT(mapping_table,
                          ::LIBC_NAMESPACE::internal::kForkPrioMappingTable,
                          &::LIBC_NAMESPACE::internal::mapping_table_fork_reinit)
