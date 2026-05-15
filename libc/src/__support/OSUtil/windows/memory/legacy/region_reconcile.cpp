//===--- Region reconciliation — implementation ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/legacy/region_reconcile.h"

#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory {

namespace {

// Stamp [base, base + size) as FOREIGN at 64 KB granularity, but only
// over slots that are currently FREE. LIVE / PLACEHOLDER / REMAPPING
// slots already represent things we own and must not be overwritten.
//
// On already-FOREIGN slots, mark the cordon stale so the next caller
// (mmap hint, MAP_FIXED probe, or this function's reverse pass) drives
// a single coordinated MRI_Ex re-probe via revalidate_foreign().
//
// Increments stats.foreign_stamped for each slot newly cordoned and
// stats.foreign_revalidated for each FOREIGN slot we marked stale.
void stamp_foreign_holes(char *base, SIZE_T size, ReconcileStats &stats) {
  const SIZE_T gran = get_alloc_granularity();
  char *cursor = base;
  char *limit = base + size;

  while (cursor < limit) {
    SlotSnapshot snap;
    const bool occupied = g_mapping_table.snapshot(cursor, &snap);

    if (!occupied) {
      // FREE slot — install a FOREIGN stamp via the sentinel fast path
      // so subsequent hint logic skips it. Cost: one CAS + version
      // bump against FOREIGN_SENTINEL_REGION_ID.
      //
      // A failed stamp here means a peer thread published a real owned
      // slot at this address between our snapshot and our stamp — that
      // VA is now ours, not foreign, and the next reconcile pass (or the
      // running owner's lifecycle) will keep the table in sync. Treat
      // the false return as benign and move on; no telemetry bump.
      if (g_mapping_table.register_mapping_foreign(cursor, gran))
        ++stats.foreign_stamped;
    } else if (snap.region != nullptr && snap.region->skip_revalidation()) {
      // LIBC_INTERNAL / IMAGE_REGION / KERNEL_REGION — our own or
      // OS-managed VA whose lifecycle is determined outside this scan.
      // Never re-stamp these as FOREIGN; never mark them stale.
    } else if (snap.region != nullptr && snap.region->is_foreign()) {
      // Already FOREIGN. NT just confirmed (via the bulk walker that
      // drove us here) that the VA is still occupied by something we
      // don't own — the cordon is correct. Mark the slot stale so the
      // reverse pass drives a coordinated MRI_Ex through
      // revalidate_foreign(); concurrent touchers benefit from the
      // single re-probe via the state_aux probe-lock.
      g_mapping_table.mark_foreign_stale(cursor);
      ++stats.foreign_revalidated;
    }
    cursor += gran;
  }
}

// Walk the table over [base, base + size) and clear FOREIGN stamps that
// no longer correspond to NT-occupied VA. Used at the tail of a scan
// to reclaim cordons after the foreign owner has freed its placeholder.
//
// Revalidation is delegated to MappingTable::revalidate_foreign so that
// concurrent touchers (mmap hint, MAP_FIXED) coordinate through the
// state_aux probe-lock — at most one MRI_Ex syscall per slot per stale
// cycle, regardless of contention.
struct ClearStaleCtx {
  char *bound_start;
  char *bound_end;
  ReconcileStats *stats;
};

void clear_stale_cb(const SlotSnapshot *snap, void *raw_ctx) {
  auto *ctx = static_cast<ClearStaleCtx *>(raw_ctx);

  // Only target FOREIGN slots — leave LIVE / PLACEHOLDER / LIBC_
  // INTERNAL / IMAGE_REGION / KERNEL_REGION alone. Shape is the
  // authoritative test now that FOREIGN slots carry the sentinel
  // region id rather than NONE.
  if (snap->region == nullptr || !snap->region->is_foreign())
    return;

  // Drive the coordinated re-probe. revalidate_foreign returns true when
  // it cleared the cordon (NT freed the VA), false when the cordon
  // remains valid. Either outcome counts as a successful revalidation
  // pass; only the cleared case bumps foreign_cleared.
  if (g_mapping_table.revalidate_foreign(snap->view_base))
    ++ctx->stats->foreign_cleared;
}

// Extract any LIVE/PLACEHOLDER slots whose VA falls inside `[base, base+size)`.
// Used by the exec self-hollow path: after the old EXE image is unmapped,
// table slots that still reference its VA must be drained — both to clear
// the slot and to release the region reference so the descriptor returns
// to the pool. Without this, every exec would leak descriptors and the
// pool would slowly fill with dead-region entries that survive until
// process termination.
//
// stamp_foreign_holes runs after this for the same NT-FREE range, so any
// VA the loader subsequently reclaims (e.g., new image header) gets the
// usual cordon treatment via the forward pass.
struct PurgeCtx {
  unsigned extracted;
};

void purge_owned_cb(const SlotSnapshot *snap, void *raw_ctx) {
  auto *ctx = static_cast<PurgeCtx *>(raw_ctx);
  // Only owned slots — leave FOREIGN cordons to the foreign-revalidation
  // pass. (FOREIGN slots have region_id == NONE.)
  if (snap->region_id == RegionPool::NONE)
    return;
  MappingEntry e;
  if (g_mapping_table.extract(snap->view_base, &e)) {
    if (e.region_id != RegionPool::NONE)
      g_region_pool.release(e.region_id);
    ++ctx->extracted;
  }
}

unsigned purge_owned_slots_in_range(char *base, SIZE_T size) {
  PurgeCtx ctx{0};
  g_mapping_table.walk_range(base, base + size, purge_owned_cb, &ctx);
  return ctx.extracted;
}

// Core scan body shared by post_fork_scan / post_exec_scan / post_dlopen.
// `bound_start` and `bound_size` constrain the walk; pass nullptr / 0 for
// a full-VA scan. Caller MUST already hold MmapLock writer.
//
// `purge_orphans` controls the exec-only behavior: when true, NT MEM_FREE
// entries trigger a per-range scan that extracts our orphaned slots
// (drops region refs back to the pool). Fork and dlopen leave the table
// alone over MEM_FREE NT entries — fork preserves all parent VA via CoW,
// and dlopen only adds VA.
ReconcileStats reconcile_scan_locked(void *bound_start, SIZE_T bound_size,
                                     bool purge_orphans) {
  ReconcileStats stats{0, 0, 0};
  // Park on the Futex-backed remap counter until every in-flight remap
  // has retired. Caller holds MmapLock writer so no new remap can start
  // once the count first hits zero.
  g_mapping_table.wait_for_remap_drain();

  auto ws = byte_scratch(4096);
  if (!ws)
    return stats;

  // Forward pass: walk every NT region in the bound. For non-FREE NT
  // regions we stamp FOREIGN over any FREE slots we cover (cordon foreign
  // VA out of our hint/MAP_FIXED paths). For FREE NT regions, exec
  // self-hollow needs us to extract any owned slots that no longer have
  // NT backing — without this, descriptors orphaned by the hollow leak
  // until process death.
  if (bound_start != nullptr && bound_size != 0) {
    nt_pal::RegionWalker walk(bound_start, bound_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_FREE) {
        if (purge_orphans)
          purge_owned_slots_in_range(walk.chunk, walk.chunk_size);
        continue;
      }
      stamp_foreign_holes(walk.chunk, walk.chunk_size, stats);
    }
  } else {
    auto walk = nt_pal::RegionWalker::whole_process(ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_FREE) {
        if (purge_orphans)
          purge_owned_slots_in_range(walk.chunk, walk.chunk_size);
        continue;
      }
      stamp_foreign_holes(walk.chunk, walk.chunk_size, stats);
    }
  }

  // Reverse pass: clear FOREIGN stamps that no longer correspond to NT
  // state. Bounded scans only check inside the bound; full scans do the
  // whole table.
  if (bound_start != nullptr && bound_size != 0) {
    ClearStaleCtx ctx{static_cast<char *>(bound_start),
                      static_cast<char *>(bound_start) + bound_size, &stats};
    g_mapping_table.walk_range(bound_start,
                               static_cast<char *>(bound_start) + bound_size,
                               clear_stale_cb, &ctx);
  } else {
    ClearStaleCtx ctx{nullptr, nullptr, &stats};
    g_mapping_table.for_each_live(clear_stale_cb, &ctx);
  }

  return stats;
}

} // namespace

// =============================================================================
// Public entry points.
// =============================================================================

ReconcileStats post_fork_scan() {
  // Fork child is single-threaded at the time fork_reinit runs, but
  // acquiring the writer lock costs little and keeps the contract
  // identical to the multi-threaded entry points.
  MmapLockWriterGuard guard;
  return reconcile_scan_locked(nullptr, 0, /*purge_orphans=*/false);
}

ReconcileStats post_exec_scan() {
  // Self-hollow exec unmaps the old EXE image (and any other VA the
  // hollow walks tear down). Slots referencing that VA must be
  // extracted so their region descriptors return to the pool — the
  // alternative is a slow descriptor leak across exec chains.
  MmapLockWriterGuard guard;
  return reconcile_scan_locked(nullptr, 0, /*purge_orphans=*/true);
}

ReconcileStats post_dlopen_reconcile(void *bound_start, SIZE_T bound_size) {
  MmapLockWriterGuard guard;
  return reconcile_scan_locked(bound_start, bound_size,
                               /*purge_orphans=*/false);
}

unsigned cordon_foreigners_in_range(void *base, SIZE_T size) {
  // Caller already holds MmapLock writer (asserted indirectly by every
  // table mutation we drive below — register_foreign tolerates concurrent
  // foreign restamps but expects no concurrent owned-slot publish on the
  // same VA, which the writer lock guarantees).
  if (base == nullptr || size == 0)
    return 0;

  auto ws = byte_scratch(4096);
  if (!ws)
    return 0;

  ReconcileStats stats{0, 0, 0};
  nt_pal::RegionWalker walk(base, size, ws.data(), ws.size());
  while (walk.next()) {
    if (walk.entry->State == MEM_FREE)
      continue;
    // stamp_foreign_holes already does the right thing: skip owned
    // slots, stamp FREE holes, mark already-FOREIGN slots stale. We
    // don't run the reverse pass here — the caller is about to tear
    // the range down or replace it, so cleared stale cordons would
    // immediately become irrelevant.
    stamp_foreign_holes(walk.chunk, walk.chunk_size, stats);
  }
  return stats.foreign_stamped;
}

} // namespace memory
} // namespace windows

// ============================================================================
// libc internal entry points — fork / exec.
// ============================================================================
//
// These pair the RegionPool fork_reinit (which clears pool-internal locks
// dead parent threads may have held) with a full-VA reconcile sweep.
// Callers (libc_fork_reinit_impl, exec_self_hollow) invoke them AFTER
// mmap_lock_fork_reinit so the bulk scan can acquire MmapLock writer.
//
// Discarding the ReconcileStats is intentional: telemetry is an aid for
// tests, not a fork-time error path. A reconcile pass cannot fail in a
// way the child can usefully recover from — if the bulk-VA syscall is
// unavailable the table simply stays unchanged and the engine layer
// re-stamps cordons lazily on next touch.

namespace internal {

void memory_reconcile_fork_reinit() {
  windows::memory::g_region_pool.fork_reinit();
  (void)windows::memory::post_fork_scan();
}

void memory_reconcile_exec_reinit() {
  windows::memory::g_region_pool.fork_reinit();
  (void)windows::memory::post_exec_scan();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FORK_REINIT(memory_reconcile,
                          ::LIBC_NAMESPACE::internal::kForkPrioMemoryReconcile,
                          &::LIBC_NAMESPACE::internal::memory_reconcile_fork_reinit)
