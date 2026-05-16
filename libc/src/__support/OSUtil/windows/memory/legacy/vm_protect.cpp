//===---------- Windows munmap/mprotect engine (kernel functions) ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX munmap and mprotect implementation for Windows.
//
// munmap — three dispatch paths based on region type:
//
//   MEM_PRIVATE (anonymous mmap):
//     Full allocation:  NtFreeVirtualMemory(MEM_RELEASE) with size=0.
//     Partial committed: nt_pal::interior_release (MEM_RELEASE with non-zero size)
//       punches the sub-range to MEM_FREE in a single syscall. Surviving
//       fragments become independent allocations.
//     Partial placeholder: split at page boundaries, release target.
//
//   MEM_MAPPED (file/pagefile views):
//     Full view:        NtUnmapViewOfSectionEx + release placeholder
//     Partial view:     Split-remap algorithm — unmap entire view to
//                       placeholder, split at page-aligned boundaries,
//                       remap kept fragments from the original section.
//                       VEH remap guard stalls concurrent faulting threads
//                       during the window.
//
//   MEM_IMAGE: rejected (loaded PE images, not from our mmap).
//
// mprotect — Windows NtProtectVirtualMemory operates on a single allocation
// region per call. POSIX mprotect operates on arbitrary page-aligned ranges
// that may span multiple regions. This implementation dispatches via the
// mapping table snapshot (shape-driven recipe per region); when a slot is
// missing the outer loop falls through to a single MBI probe to classify
// foreign memory before applying protection per-region.
//
// State transitions for each region chunk:
//   Placeholder + PROT_NONE:           no-op (already inaccessible)
//   Placeholder + accessible:          create section + map (demand-map)
//   SEC_RESERVE uncommitted + PROT_NONE: no-op (already inaccessible)
//   SEC_RESERVE uncommitted + accessible: NtAllocateVirtualMemoryEx(MEM_COMMIT)
//   Committed + any:                   NtProtectVirtualMemory
//
// Three uncommitted-page cases:
//   MEM_PRIVATE (bare placeholder from mmap PROT_NONE): create section + map.
//   MEM_MAPPED (SEC_RESERVE view from MAP_NORESERVE): commit with new prot.
//   Both become committed; NtProtectVirtualMemory handles further changes.
//
//===----------------------------------------------------------------------===//

#include "vm_protect.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/syscall_return.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/legacy/anon_region_ops.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/remap_transaction.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/numa_policy.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_snapshot.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/errno_macros.h"

#ifdef LIBC_TARGET_ARCH_IS_X86_64
#include "src/__support/OSUtil/windows/security/pkey_state.h"
#endif

namespace LIBC_NAMESPACE_DECL {

namespace {

//==========================================================================
// Placeholder release helper
//==========================================================================

/// Release a freed placeholder iff no live mapping abuts on either side.
///
/// Replaces the legacy `coalesce_or_release_placeholder` whose two MRI
/// probes carried a cross-thread merge hazard: if a concurrent mmap had
/// just reserved the abutting placeholder but not yet published its view,
/// the kernel-side coalesce we no longer perform would silently absorb it
/// into our just-freed range. The peer's later
/// `MEM_REPLACE_PLACEHOLDER` then mismatched the coalesced extent and
/// crashed with STATUS_CONFLICTING_ADDRESSES.
///
/// The mapping-table-driven check closes that class by construction:
/// `has_neighbor_*` sees only fully-published LIVE / PLACEHOLDER slots,
/// never the in-flight WRITING window. Two O(1) radix probes, zero
/// syscalls; the release itself is the only NT call on the hot path and
/// only when the range is genuinely isolated.
LIBC_INLINE void release_if_isolated(void *addr, SIZE_T size) {
  if (windows::g_mapping_table.has_neighbor_before(addr, nullptr))
    return;
  if (windows::g_mapping_table.has_neighbor_after(
          static_cast<char *>(addr) + size, nullptr))
    return;
  nt_pal::free_placeholder(addr);
}

//==========================================================================
// munmap helpers
//==========================================================================

//==========================================================================
// View extent discovery
//==========================================================================


//==========================================================================
// Private memory unmap (bare placeholders and legacy MEM_PRIVATE)
//==========================================================================

/// Unmap a MEM_PRIVATE region (bare placeholder or committed private memory).
///
/// This is the table-miss fallback path — used when the caller could not
/// locate a region descriptor in the mapping table (e.g., legacy untracked
/// memory). Shape-driven dispatch in `unmap_anon_placeholder` is the
/// preferred path; this only runs when snapshot returns no region.
///
/// Full release: NtFreeVirtualMemory(MEM_RELEASE) with size=0.
/// Partial committed: nt_pal::interior_release punches sub-range to MEM_FREE.
/// Partial placeholder: split at page boundaries and release target.
///
/// Returns 0 on success, -errno on failure.
long unmap_private_chunk(HANDLE process, char *chunk_start,
                                     char *chunk_end, void *alloc_base,
                                     bool is_committed) {
  MEMORY_BASIC_INFORMATION end_mbi;
  bool end_beyond_alloc =
      !nt_pal::query_region(chunk_end, end_mbi) ||
      (end_mbi.AllocationBase != alloc_base);

  // Full release: covers entire allocation.
  if (chunk_start == static_cast<char *>(alloc_base) && end_beyond_alloc) {
    PVOID base = chunk_start;
    SIZE_T region_size = 0;
    NTSTATUS status =
        ::NtFreeVirtualMemory(process, &base, &region_size, MEM_RELEASE);
    if (NT_SUCCESS(status))
      return 0;
    return windows_util::ntstatus_to_kerr(status);
  }

  // Partial unmap.
  SIZE_T target_size =
      static_cast<SIZE_T>(chunk_end - chunk_start);

  if (is_committed) {
    // Committed private memory: nt_pal::interior_release punches the sub-range
    // directly to MEM_FREE in a single syscall. VA is returned to the
    // system; surviving fragments become independent allocations.
    // Replaces the three-step decommit+preserve+coalesce (3-7 syscalls).
    if (!nt_pal::interior_release(chunk_start, target_size))
      return -EINVAL;
    return 0;
  }

  // Bare placeholder: split at page boundaries, release target.
  uintptr_t target_start = reinterpret_cast<uintptr_t>(chunk_start);
  uintptr_t target_end = reinterpret_cast<uintptr_t>(chunk_end);
  uintptr_t ab = reinterpret_cast<uintptr_t>(alloc_base);

  if (target_start < ab)
    target_start = ab;

  if (target_start > ab) {
    if (!nt_pal::split_placeholder(alloc_base, target_start - ab))
      return -EINVAL;
  }

  char *alloc_end = nt_pal::find_alloc_end(alloc_base);
  uintptr_t ae = reinterpret_cast<uintptr_t>(alloc_end);
  if (target_end > ae || target_end == 0)
    target_end = ae;

  if (target_end < ae) {
    char *split_base = reinterpret_cast<char *>(target_start);
    SIZE_T split_off = target_end - target_start;
    if (!nt_pal::split_placeholder(split_base, split_off))
      return -EINVAL;
  }

  release_if_isolated(reinterpret_cast<void *>(target_start),
                      target_end - target_start);
  return 0;
}

//==========================================================================
// Shape-driven unmap recipes for tracked anonymous regions.
//==========================================================================

/// Unmap a tracked ANON_PLACEHOLDER region.
///
/// Three sub-flavors — routed by region flags:
///   * COMMITTED  (placeholder-committed to MEM_PRIVATE, kernel-identical
///                 to the retired ANON_ONESHOT):
///       Full   — MEM_RELEASE on the view_base; slot extracted first.
///       Partial — single-syscall MEM_DECOMMIT. Slot stays LIVE; subsequent
///                 access to the decommitted sub-range faults (correct
///                 POSIX munmap semantics) and re-mmap via nt_pal::commit_in_reservation_no_writewatch works.
///   * NORESERVE (demand-commit via VEH) / PROT_NONE (bare placeholder):
///       Full   — nt_pal::free_va (MEM_RELEASE) on the view_base.
///       Partial — nt_pal::split_placeholder at the chunk bounds, release the
///                 carve-out, re-publish surviving head/tail via
///                 trim_anon_slot_for_hole.
long unmap_anon_placeholder(char *chunk_start, char *chunk_end,
                            const windows::SlotSnapshot &snap) {
  void *region_base = snap.view_base;
  char *region_end =
      static_cast<char *>(region_base) + snap.view_size;
  const bool is_full =
      chunk_start == static_cast<char *>(region_base) &&
      chunk_end >= region_end;
  const bool committed_flag =
      snap.region->has_flag(windows::memory::region_flag::COMMITTED);
  const bool noreserve_flag =
      snap.region->has_flag(windows::memory::region_flag::NORESERVE);

  if (is_full) {
    // Capture pre-extract state so rollback re-registers correctly.
    // PROT_NONE (state=PLACEHOLDER) needs register_placeholder on rollback;
    // COMMITTED / NORESERVE (state=LIVE) need register_mapping.
    const bool was_placeholder_state = !committed_flag && !noreserve_flag;
    windows::MappingEntry entry;
    if (!windows::g_mapping_table.extract(region_base, &entry))
      return -EINVAL;

    if (!nt_pal::free_va(region_base)) {
      bool republished =
          was_placeholder_state
              ? windows::g_mapping_table.register_placeholder(
                    region_base, entry.view_size, entry.region_id,
                    entry.alloc_id)
              : windows::g_mapping_table.register_mapping(
                    region_base, entry.view_size, entry.region_id,
                    entry.alloc_id, entry.view_prot, entry.flags);
      if (!republished &&
          entry.region_id != windows::memory::RegionPool::NONE)
        windows::memory::g_region_pool.release(entry.region_id);
      return -EINVAL;
    }
    if (entry.region_id != windows::memory::RegionPool::NONE)
      windows::memory::g_region_pool.release(entry.region_id);
    return 0;
  }

  // Partial unmap.
  if (committed_flag) {
    // Single-syscall MEM_DECOMMIT. Slot remains LIVE; subsequent access
    // to the decommitted sub-range faults, matching POSIX munmap.
    SIZE_T sz = static_cast<SIZE_T>(chunk_end - chunk_start);
    if (!nt_pal::decommit_private_range(chunk_start, sz))
      return -EINVAL;
    return 0;
  }

  // PROT_NONE / NORESERVE partial: split the placeholder at the chunk's
  // page-aligned bounds and release the carved-out range.
  uintptr_t rb = reinterpret_cast<uintptr_t>(region_base);
  uintptr_t cs = reinterpret_cast<uintptr_t>(chunk_start);
  uintptr_t ce = reinterpret_cast<uintptr_t>(chunk_end);

  if (cs > rb) {
    if (!nt_pal::split_placeholder(region_base, cs - rb))
      return -EINVAL;
  }
  uintptr_t re = reinterpret_cast<uintptr_t>(region_end);
  if (ce < re) {
    if (!nt_pal::split_placeholder(reinterpret_cast<void *>(cs), ce - cs))
      return -EINVAL;
  }

  release_if_isolated(reinterpret_cast<void *>(cs),
                      static_cast<SIZE_T>(ce - cs));

  // Re-publish surviving head/tail fragments under fresh region IDs.
  (void)windows::trim_anon_slot_for_hole(region_base, cs, ce);
  return 0;
}

//==========================================================================
// File view full unmap
//==========================================================================

/// Unmap an entire file view and release its address space.
///
/// Uses extract (not remove) so the region reference survives until the
/// NT unmap succeeds. On failure the slot is re-published from the same
/// region reference — no orphaned mappings, no double-release of the
/// region's section/file handles.
///
/// Returns 0 on success, -errno on failure.
long full_unmap_view(HANDLE /*process*/, char *view_base) {
  windows::MappingEntry entry;
  if (!windows::g_mapping_table.extract(view_base, &entry)) {
    // No table entry — still try the unmap (best-effort).
    NTSTATUS st = nt_pal::unmap_view_preserve(view_base);
    if (NT_ERROR(st))
      return windows_util::ntstatus_to_kerr(st);
    // Untracked view — we have no slot recording its extent. The unmap
    // succeeded so the placeholder is in our hand; nt_pal::free_placeholder is
    // size-agnostic (NT looks up the placeholder's recorded length from
    // the VAD), so we don't need an MBI probe to find the bounds.
    nt_pal::free_placeholder(view_base);
    return 0;
  }

  SIZE_T view_size = entry.view_size;

  NTSTATUS st = nt_pal::unmap_view_preserve(view_base);
  if (NT_ERROR(st)) {
    // NT unmap failed — re-publish the entry. register_mapping consumes
    // the region ref we already own from extract().
    if (windows::g_mapping_table.register_mapping(
            view_base, entry.view_size, entry.region_id, entry.alloc_id,
            entry.view_prot, entry.flags))
      return windows_util::ntstatus_to_kerr(st);

    // Re-publish failed too. The view is still mapped in NT and the
    // table no longer tracks it — exactly the silent-VA-drift class the
    // redesign was written to eliminate. Force a hard teardown:
    // nt_pal::unmap_view (no placeholder preservation) returns the VA to
    // MEM_FREE; nt_pal::free_placeholder is a no-op-or-cleanup safety net for
    // the case where the view sat inside a placeholder we still need to
    // surrender. Either path leaves the VA reclaimable and drops the
    // region reference we held — no orphan NT view, no leaked descriptor
    // refcount.
    NTSTATUS hard = nt_pal::unmap_view(view_base);
    if (NT_ERROR(hard))
      nt_pal::free_placeholder(view_base);
    if (entry.region_id != windows::memory::RegionPool::NONE)
      windows::memory::g_region_pool.release(entry.region_id);
    return windows_util::ntstatus_to_kerr(st);
  }

  // Success — release the region ref we hold; the pool closes the
  // section/file handles when refcount drops to zero.
  if (entry.region_id != windows::memory::RegionPool::NONE)
    windows::memory::g_region_pool.release(entry.region_id);

  release_if_isolated(view_base, view_size);
  return 0;
}

//==========================================================================
// File view partial unmap (split-remap)
//==========================================================================
//
// NtUnmapViewOfSectionEx always unmaps the entire view. To partially unmap
// a file view while preserving adjacent content:
//
//   1. Snapshot per-sub-region protections of kept fragments
//   2. Unmap the entire view → single placeholder
//   3. Split the placeholder at the kept-fragment boundaries
//   4. Remap kept fragments from the original section handle
//   5. Replay mprotect differences onto remapped fragments
//   6. Coalesce freed placeholder with neighbors, release if isolated
//
// The VEH remap guard covers the window between steps 2 and 5: any thread
// faulting on the view's address range stalls until the remap completes.
//
// Split points must be page-aligned (4KB on x64). Placeholder splits
// are page-granular, matching Linux's page-exact munmap semantics.

/// Partial unmap of a section-backed view via transactional split-remap.
///
/// Caller dispatches on RegionShape; this routine covers all section-backed
/// shapes uniformly (FILE_VIEW_MONO, FILE_VIEW_CHUNKED, FILE_VIEW_RESERVE,
/// ANON_RESERVE_SECTION). The shape-specific bookkeeping
/// happens *inside* RemapTransaction::commit, which:
///   * MONO     — promotes shape to CHUNKED and seeds the chunk list with
///                the surviving head/tail fragments
///   * CHUNKED  — punches the freed range out of the existing chunk list
///                via chunk_list_punch (held briefly under chunk_list_lock)
///   * RESERVE shapes — same flow; demand-commit faults are quiesced by
///                the REMAPPING slot's guard array during the window
///
/// On failure the transaction destructor rolls the slot back to LIVE with
/// no NT-level mutation visible to other threads (REMAPPING covers the
/// window; faulters stall on the guard).
///
/// `unmap_start`/`unmap_end` are page-aligned addresses within
/// [view_base, view_end). Returns 0 on success, -errno on failure.
long partial_unmap_view(HANDLE /*process*/, char *view_base,
                                    char *view_end, uintptr_t unmap_start,
                                    uintptr_t unmap_end) {
  uintptr_t vb = reinterpret_cast<uintptr_t>(view_base);
  uintptr_t ve = reinterpret_cast<uintptr_t>(view_end);

  windows::RemapTransaction txn(view_base, view_end, unmap_start,
                                unmap_end - unmap_start);

  bool stale = false;
  if (!txn.prepare(&stale)) {
    // Stale snapshot: the slot's extent shrunk between our caller's
    // snapshot and our begin_remap CAS. Caller must re-snapshot — propagate
    // -EAGAIN as an internal sentinel (never reaches the user; the munmap
    // outer loop catches it and retries the same `current` cursor).
    if (stale)
      return -EAGAIN;
    return -EINVAL;
  }

  // Large-page views (no file/section backing) can't be partially unmapped
  // — indivisible. The REMAPPING slot rolls back on txn destruction.
  if (txn.region() == nullptr ||
      txn.region()->section_handle == nullptr)
    return -EINVAL;

  auto result = txn.execute();
  if (!result.target)
    return -ENOMEM; // ~txn rolls back.

  // Release the carved-out placeholder if no live neighbor abuts it.
  // Prevents sub-64KB unrepairable VA gaps without inviting the
  // cross-thread coalesce hazard the old MRI-driven helper carried.
  auto [freed_base, freed_size] = result.target.consume();
  release_if_isolated(freed_base, freed_size);

  // Same release decision for the placeholders left behind by failed
  // fragment remaps — the views never re-published, so each fragment is
  // its own carved-out range.
  if (txn.has_left() && !result.left_ok) {
    SIZE_T left_size = unmap_start - vb;
    release_if_isolated(view_base, left_size);
  }
  if (txn.has_right() && !result.right_ok) {
    SIZE_T right_size = ve - unmap_end;
    release_if_isolated(reinterpret_cast<void *>(unmap_end), right_size);
  }

  if (!txn.commit())
    return -ENOMEM;

  if (LIBC_UNLIKELY((txn.has_left() && !result.left_ok) ||
                    (txn.has_right() && !result.right_ok)))
    return -ENOMEM;

  return 0;
}

//==========================================================================
// mprotect helpers
//==========================================================================

/// Demand-map a placeholder region: replace with private committed memory.
/// Used when mprotect makes a PROT_NONE mmap (bare placeholder) accessible.
/// Private memory (not section-backed) so MEM_DECOMMIT works for MADV_DONTNEED
/// and partial munmap uses decommit+punch. No mapping table entry needed.
///
/// Consults the thread's NUMA policy (set_mempolicy) to select a NUMA node
/// for physical page placement. First-touch via MADV_POPULATE_WRITE reinforces
/// the placement on the calling thread's node.
long demand_map_placeholder(HANDLE /*process*/, void *addr, SIZE_T size,
                                        DWORD prot) {
  int node = windows::select_numa_node();
  if (node >= 0) {
    NTSTATUS st = nt_pal::commit_replace_numa(addr, size, prot,
                                                static_cast<ULONG>(node));
    if (NT_SUCCESS(st))
      return 0;
    // NUMA hint failed — fall through to non-NUMA commit.
  }

  NTSTATUS st = nt_pal::commit_replace(addr, size, prot);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  return 0;
}

/// Whole-region protection skipping the per-page MBI walk.
///
/// Applies only to regions whose shape guarantees every page in the range is
/// already committed (no demand-commit, no SEC_RESERVE backing). The caller
/// must have verified that via `is_fully_committed_shape()` before reaching
/// this path; otherwise NtProtectVirtualMemory will fail with
/// STATUS_NOT_COMMITTED on the first uncommitted page.
///
/// WRITECOPY translation is driven by `RegionDesc.flags & REGION_FLAG_COW`
/// rather than `mbi.AllocationProtect`. The flag is stamped at acquire-time
/// from the original mmap(MAP_PRIVATE) decision and is immutable for the
/// region's lifetime — no per-call NT query needed.
///
/// VM_FLAG_PROT_CHANGED book-keeping uses the region's section-backed
/// predicate (file views and SEC_RESERVE anon-section regions) so the VEH
/// demand-commit fast path takes the safe slow path on subsequent faults
/// against this view.
///
/// Returns 0 on success, -errno on failure.
long protect_region(HANDLE process, void *addr, SIZE_T size, DWORD new_prot,
                    int prot, const windows::memory::RegionDesc &region,
                    void *region_base) {
  // Stamp PROT_CHANGED before the kernel transition. VEH demand-commit reads
  // this flag on every fault; setting it after NtProtectVirtualMemory would
  // race with a fault that arrives between the kernel state change and the
  // flag store. Only meaningful for section-backed regions whose VEH path
  // honors view_prot.
  if (region.is_section_backed())
    windows::g_mapping_table.add_flags(region_base,
                                       windows::VM_FLAG_PROT_CHANGED);

  // PAGE_WRITECOPY translation for MAP_PRIVATE file views: the backing
  // section may have been created with only read access (read-only fd), so
  // a direct PAGE_READWRITE request would fail with STATUS_SECTION_PROTECTION.
  // PAGE_WRITECOPY lets writes trigger the kernel's CoW path without needing
  // write access to the section itself. Driven by the region flag stamped at
  // acquire — the answer was decided at mmap time and is immutable.
  DWORD effective_prot = new_prot;
  if (region.is_cow()) {
    if (effective_prot == PAGE_READWRITE)
      effective_prot = PAGE_WRITECOPY;
    else if (effective_prot == PAGE_EXECUTE_READWRITE)
      effective_prot = PAGE_EXECUTE_WRITECOPY;
  }

  PVOID base = addr;
  SIZE_T region_size = size;
  ULONG old_protect;
  NTSTATUS status = ::NtProtectVirtualMemory(process, &base, &region_size,
                                             effective_prot, &old_protect);

  // CFG-secured / MmSecureVirtualMemory cache invalidation retry. Same as
  // the per-chunk path; both apply when the kernel cached a stale protection
  // state for this VA.
  if (status == STATUS_INVALID_PAGE_PROTECTION &&
      process == NtCurrentProcess()) {
    if (::RtlFlushSecureMemoryCache(addr, size)) {
      base = addr;
      region_size = size;
      status = ::NtProtectVirtualMemory(process, &base, &region_size,
                                        effective_prot, &old_protect);
    }
  }
  if (NT_ERROR(status)) {
    if (region.is_section_backed())
      windows::g_mapping_table.remove_flags(region_base,
                                            windows::VM_FLAG_PROT_CHANGED);
    return windows_util::ntstatus_to_kerr(status);
  }

  // ARM64: flush I-cache after making pages executable (no-op on x86_64).
  if (prot & PROT_EXEC)
    ::NtFlushInstructionCache(process, addr, size);

  return 0;
}

/// True for shapes whose VA is fully committed end-to-end with no possibility
/// of demand-commit pages in the middle. Drives the single-syscall mprotect
/// fast path: when the request range falls inside one such region, the bulk
/// MBI walk is skipped entirely.
LIBC_INLINE bool is_fully_committed_shape(windows::memory::RegionShape shape,
                                          uint16_t flags) {
  using S = windows::memory::RegionShape;
  if (flags & windows::memory::region_flag::NORESERVE)
    return false;
  switch (shape) {
  case S::ANON_PLACEHOLDER:
    // PROT_NONE (neither flag) is not committed; only COMMITTED placeholder
    // VA is a single-syscall mprotect target. The shape alone is not
    // sufficient — the COMMITTED bit distinguishes LIVE-committed from
    // PROT_NONE PLACEHOLDER state.
    return (flags & windows::memory::region_flag::COMMITTED) != 0;
  case S::FILE_VIEW_MONO:
  case S::FILE_VIEW_CHUNKED:
    return true;
  default:
    return false;
  }
}

/// Apply protection to a single region chunk, dispatching on state.
///
/// `region` is the resolved RegionDesc for this chunk's view (from
/// g_mapping_table.snapshot at AllocationBase) — null when the chunk
/// belongs to memory we don't track (foreign sections, image VADs from
/// the loader). When non-null, the WRITECOPY translation reads the
/// durable REGION_FLAG_COW bit decided at mmap time. When null, fall
/// back to mbi.AllocationProtect — only relevant for image VADs and
/// foreign section views, where the kernel performs the translation
/// itself in most cases.
///
/// Returns 0 on success, -errno on failure.
long protect_chunk(HANDLE process, void *addr, SIZE_T size,
                               DWORD new_prot, int prot,
                               const MEMORY_BASIC_INFORMATION &mbi,
                               const windows::memory::RegionDesc *region) {
  const bool is_committed = (mbi.State == MEM_COMMIT);

  if (!is_committed && prot != PROT_NONE) {
    // SEC_RESERVE section view (MAP_NORESERVE): uncommitted pages within a
    // mapped section. Commit with the requested protection — no need to
    // create a new section, one already backs this view.
    if (mbi.Type == MEM_MAPPED) {
      NTSTATUS st = nt_pal::commit_in_reservation_no_writewatch(addr, size,
                                                                  new_prot);
      if (NT_ERROR(st))
        return windows_util::ntstatus_to_kerr(st);
      return 0;
    }

    // Bare placeholder (MEM_PRIVATE): replace with committed private memory.
    // Clean up any stale mapping table entry first — if this address was
    // previously a SEC_RESERVE section view that got unmapped (leaving a
    // placeholder), the entry is now dangling. Safe no-op when no entry exists.
    windows::g_mapping_table.remove(mbi.AllocationBase);
    return demand_map_placeholder(process, addr, size, new_prot);
  }

  // Placeholder/reserved + PROT_NONE: already inaccessible, nothing to do.
  if (!is_committed)
    return 0;

  // Mark the mapping BEFORE the protection change. The VEH demand-commit
  // path checks this flag to decide between the fast path (commit with
  // view_prot) and the safe path (NtQueryVirtualMemory state check).
  // Setting it before NtProtectVirtualMemory closes the race where another
  // thread faults between the kernel protection change and the flag store.
  if (mbi.Type == MEM_MAPPED)
    windows::g_mapping_table.add_flags(mbi.AllocationBase,
                                        windows::VM_FLAG_PROT_CHANGED);

  // Committed: change protection. PAGE_NOACCESS preserves page contents,
  // so mprotect(PROT_NONE) + mprotect(PROT_READ|PROT_WRITE) round-trips.
  //
  // MEM_IMAGE (loaded PE sections): the kernel silently substitutes
  // PAGE_WRITECOPY for PAGE_READWRITE and PAGE_EXECUTE_WRITECOPY for
  // PAGE_EXECUTE_READWRITE on image VADs. Writes trigger COW; the shared
  // section is never modified. We do not reject MEM_IMAGE — this matches
  // Linux behavior and is required for JIT patching.
  //
  // MAP_PRIVATE file views need the WRITECOPY translation for
  // PAGE_READWRITE / PAGE_EXECUTE_READWRITE — the backing section may have
  // been created with only read access (read-only fd), so a direct RW
  // request would fail with STATUS_SECTION_PROTECTION. WRITECOPY lets
  // writes trigger the kernel's CoW path without needing write access to
  // the section.
  //
  // Source of truth: REGION_FLAG_COW on the resolved RegionDesc, decided
  // at mmap time and immutable for the region's lifetime. mbi.Allocation
  // Protect is only consulted when no region is tracked (foreign view).
  DWORD effective_prot = new_prot;
  bool needs_cow_translation;
  if (region != nullptr) {
    needs_cow_translation = region->is_cow();
  } else if (mbi.Type == MEM_MAPPED) {
    DWORD alloc_prot = mbi.AllocationProtect & 0xFF;
    needs_cow_translation = (alloc_prot == PAGE_WRITECOPY ||
                             alloc_prot == PAGE_EXECUTE_WRITECOPY);
  } else {
    needs_cow_translation = false;
  }
  if (needs_cow_translation) {
    if (effective_prot == PAGE_READWRITE)
      effective_prot = PAGE_WRITECOPY;
    else if (effective_prot == PAGE_EXECUTE_READWRITE)
      effective_prot = PAGE_EXECUTE_WRITECOPY;
  }

  // Failure codes specific to protection changes:
  //   STATUS_SECTION_PROTECTION    → EACCES (prot exceeds section access mask)
  //   STATUS_DYNAMIC_CODE_BLOCKED  → EACCES (ACG active; W^X policy violation)
  // Both are mapped in ntstatus_to_errno.
  PVOID base = addr;
  SIZE_T region_size = size;
  ULONG old_protect;
  NTSTATUS status = ::NtProtectVirtualMemory(process, &base, &region_size,
                                             effective_prot, &old_protect);
  // STATUS_INVALID_PAGE_PROTECTION retry: CFG-secured ranges and regions
  // locked by a kernel driver via MmSecureVirtualMemory cache the protection
  // state. RtlFlushSecureMemoryCache invalidates the cache and lets the kernel
  // retry the protection change. Not applicable to ordinary MEM_IMAGE sections
  // (those produce STATUS_SECTION_PROTECTION on incompatible access, which
  // does not benefit from a flush).
  if (status == STATUS_INVALID_PAGE_PROTECTION &&
      process == NtCurrentProcess()) {
    if (::RtlFlushSecureMemoryCache(addr, size)) {
      base = addr;
      region_size = size;
      status = ::NtProtectVirtualMemory(process, &base, &region_size,
                                        new_prot, &old_protect);
    }
  }
  if (NT_ERROR(status)) {
    // Clear VM_FLAG_PROT_CHANGED on failure — leaving it set permanently
    // forces all future VEH faults on this view through the slow
    // NtQueryVirtualMemory path instead of the fast commit path.
    if (mbi.Type == MEM_MAPPED)
      windows::g_mapping_table.remove_flags(mbi.AllocationBase,
                                             windows::VM_FLAG_PROT_CHANGED);
    return windows_util::ntstatus_to_kerr(status);
  }

  // ARM64: flush I-cache after making pages executable (no-op on x86_64).
  if (prot & PROT_EXEC)
    ::NtFlushInstructionCache(process, addr, size);

  return 0;
}

} // namespace

// ===========================================================================
// Kernel function — implements Linux SYS_munmap semantics in userspace.
// Returns 0 on success, -errno on failure. Called from syscall_impl()
// dispatch and from the POSIX entry point wrapper below.
// ===========================================================================

namespace internal {

intptr_t munmap(void *addr, size_t size) {
  if (LIBC_UNLIKELY(!addr || size == 0))
    return -EINVAL;

  if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
    return -EINVAL;

  const SIZE_T rounded_size = windows::round_to_page(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -EINVAL;
  HANDLE current_process = NtCurrentProcess();

  // Shared lock serializes against MAP_FIXED's prepare_for_fixed, which
  // does a multi-step MBI walk + extract + unmap that can race with us.
  // Multiple concurrent munmaps proceed in parallel (shared vs shared);
  // per-slot CAS in mapping_table.h handles same-view contention.
  windows::g_mmap_lock.acquire_shared();

  //==========================================================================
  // Iterate through all regions in [addr, addr+rounded_size).
  // The range may span multiple allocations (private, file-mapped, free).
  //==========================================================================
  intptr_t result = 0;
  char *current = static_cast<char *>(addr);
  char *end = current + rounded_size;

  while (current < end) {
    //======================================================================
    // Shape-driven dispatch — preferred path. Snapshot the table at
    // `current` and route by RegionShape. Falls back to MBI dispatch only
    // when the address has no slot (untracked / foreign memory).
    //
    // The snapshot uses radix probes from the slot key (view_base aligned
    // down to alloc granularity) so we look up the slot covering `current`.
    //======================================================================
    char *aligned_current = reinterpret_cast<char *>(
        windows::align_down_to_granularity(
            reinterpret_cast<uintptr_t>(current)));
    windows::SlotSnapshot snap;
    bool have_snap =
        windows::g_mapping_table.snapshot(aligned_current, &snap) &&
        snap.region != nullptr;

    if (have_snap) {
      char *region_base = static_cast<char *>(snap.view_base);
      char *region_end = region_base + snap.view_size;
      char *chunk_start = (current > region_base) ? current : region_base;
      char *chunk_end_eff = (end < region_end) ? end : region_end;

      // Page-align the chunk bounds (placeholder ops require page alignment).
      uintptr_t cs = windows::align_down_to_page(
          reinterpret_cast<uintptr_t>(chunk_start));
      uintptr_t ce = windows::align_up_to_page(
          reinterpret_cast<uintptr_t>(chunk_end_eff));
      uintptr_t rb = reinterpret_cast<uintptr_t>(region_base);
      uintptr_t re = reinterpret_cast<uintptr_t>(region_end);
      if (cs < rb)
        cs = rb;
      if (ce > re || ce == 0)
        ce = re;

      const windows::memory::RegionShape shape = snap.region->current_shape();
      switch (shape) {
      case windows::memory::RegionShape::ANON_PLACEHOLDER: {
        result = unmap_anon_placeholder(reinterpret_cast<char *>(cs),
                                        reinterpret_cast<char *>(ce), snap);
        break;
      }
      case windows::memory::RegionShape::FILE_VIEW_MONO:
      case windows::memory::RegionShape::FILE_VIEW_CHUNKED:
      case windows::memory::RegionShape::FILE_VIEW_RESERVE:
      case windows::memory::RegionShape::ANON_RESERVE_SECTION: {
        bool is_full = (cs == rb && ce == re);
        if (is_full)
          result = full_unmap_view(current_process, region_base);
        else
          result = partial_unmap_view(current_process, region_base, region_end,
                                      cs, ce);
        break;
      }
      case windows::memory::RegionShape::LIBC_INTERNAL:
      case windows::memory::RegionShape::IMAGE_REGION:
      case windows::memory::RegionShape::KERNEL_REGION:
        result = -EINVAL;
        break;
      case windows::memory::RegionShape::FOREIGN_SENTINEL:
        result = -EINVAL;
        break;
      case windows::memory::RegionShape::NONE:
        // Sentinel — should not appear for a snap with non-null region.
        // Fall through to MBI fallback as a defensive guard.
        have_snap = false;
        break;
      }

      if (have_snap) {
        // Internal-only retry signal from partial_unmap_view: the slot
        // shrunk under us between snapshot and begin_remap. Re-iterate
        // the same `current` cursor; the next snapshot reflects the new
        // region geometry. Never visible to the caller.
        if (result == -EAGAIN) {
          result = 0;
          continue;
        }
        if (result != 0)
          break;
        current = region_end;
        continue;
      }
    }

    //======================================================================
    // Table-miss fallback: MBI-driven classification. Used for foreign
    // memory and the NONE sentinel (defensive — should not occur for a
    // snap with a non-null region).
    //======================================================================
    MEMORY_BASIC_INFORMATION mbi;
    bool query_ok = nt_pal::query_region(current, mbi);

    // Free memory: skip. Linux munmap succeeds even if parts of the range
    // are already unmapped.
    if (!query_ok || mbi.State == MEM_FREE) {
      if (query_ok) {
        current = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      } else {
        result = -EINVAL;
        break;
      }
      continue;
    }

    char *region_end = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    char *chunk_end = (region_end < end) ? region_end : end;

    //======================================================================
    // Image mappings: reject (loaded PE images, not from our mmap)
    //======================================================================
    if (mbi.Type == MEM_IMAGE) {
      result = -EINVAL;
      break;
    }

    //======================================================================
    // File/PageFile section views
    //======================================================================
    if (mbi.Type == MEM_MAPPED) {
      char *view_base = static_cast<char *>(mbi.AllocationBase);
      char *view_end = nt_pal::find_alloc_end(view_base);

      // Compute the unmap range within this view, expanded to page
      // boundaries. Placeholder splits are page-granular.
      uintptr_t vb = reinterpret_cast<uintptr_t>(view_base);
      uintptr_t ve = reinterpret_cast<uintptr_t>(view_end);
      uintptr_t req_start = reinterpret_cast<uintptr_t>(
          current > view_base ? current : view_base);
      uintptr_t req_end = reinterpret_cast<uintptr_t>(
          end < view_end ? end : view_end);

      uintptr_t unmap_start = windows::align_down_to_page(req_start);
      uintptr_t unmap_end = windows::align_up_to_page(req_end);
      if (unmap_start < vb)
        unmap_start = vb;
      if (unmap_end > ve || unmap_end == 0)
        unmap_end = ve;

      bool is_full = (unmap_start == vb && unmap_end == ve);

      if (is_full) {
        result = full_unmap_view(current_process, view_base);
        if (result != 0)
          break;
      } else {
        result = partial_unmap_view(current_process, view_base, view_end,
                                    unmap_start, unmap_end);
        // -EAGAIN: stale view bounds from this MBI query (a concurrent
        // partial-unmap repartitioned the section). Re-iterate the same
        // cursor; MBI re-query will pick up the new AllocationBase.
        if (result == -EAGAIN) {
          result = 0;
          continue;
        }
        if (result != 0)
          break;
      }

      current = view_end;
      continue;
    }

    //======================================================================
    // Private memory: committed anonymous mappings, bare placeholders
    // (PROT_NONE), and large-page anonymous allocations.
    //======================================================================
    if (mbi.Type == MEM_PRIVATE) {
      result = unmap_private_chunk(current_process, current, chunk_end,
                                   mbi.AllocationBase,
                                   mbi.State == MEM_COMMIT);
      if (result != 0)
        break;
      current = chunk_end;
      continue;
    }

    // Unknown region type.
    result = -EINVAL;
    break;
  }

  windows::g_mmap_lock.release_shared();
  return result;
}

// ===========================================================================
// Kernel function — implements Linux SYS_mprotect semantics in userspace.
// Returns 0 on success, -errno on failure. Called from syscall_impl()
// dispatch and from the POSIX entry point wrapper below.
// ===========================================================================

intptr_t mprotect(void *addr, size_t size, int prot) {
  // Zero-length is a no-op regardless of address (matches Linux behavior
  // where mprotect(NULL, 0, PROT_NONE) succeeds).
  if (size == 0)
    return 0;

  if (LIBC_UNLIKELY(!addr))
    return -EINVAL;

  if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
    return -EINVAL;

  // Reject unknown protection bits (matches Linux EINVAL).
  if (LIBC_UNLIKELY((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0))
    return -EINVAL;

  // W^X: mprotect never permits simultaneous W+X.  The caller must use the
  // write-then-flip pattern: mmap(RW) → write → mprotect(RX).
  if (LIBC_UNLIKELY((prot & PROT_WRITE) && (prot & PROT_EXEC)))
    return -EACCES;

  // Windows has no PAGE_EXECUTE-only protection. PROT_EXEC without PROT_READ
  // cannot be faithfully emulated — reject rather than silently promoting to
  // PAGE_EXECUTE_READ (which would weaken the caller's security intent).
  if (LIBC_UNLIKELY((prot & PROT_EXEC) && !(prot & PROT_READ)))
    return -ENOTSUP;

  const SIZE_T rounded_size = windows::round_to_page(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -ENOMEM;
  const DWORD new_prot = windows::prot_to_page_flags(prot);

  HANDLE process = NtCurrentProcess();

  // ----- Fast path: single fully-committed region -------------------------
  //
  // If the request falls entirely inside one region whose shape guarantees
  // every page is committed (ANON_PLACEHOLDER|COMMITTED, FILE_VIEW_MONO/CHUNKED with no
  // NORESERVE flag), the bulk MBI walk is pure overhead — we already know
  // the answer. One snapshot probe on the leading slot tells us shape and
  // bounds; one NtProtectVirtualMemory completes the request. Hits the
  // plan's 1-syscall budget for in-region mprotect.
  {
    char *aligned = reinterpret_cast<char *>(
        windows::align_down_to_granularity(reinterpret_cast<uintptr_t>(addr)));
    windows::SlotSnapshot snap;
    if (windows::g_mapping_table.snapshot(aligned, &snap) &&
        snap.region != nullptr) {
      char *region_base = static_cast<char *>(snap.view_base);
      char *region_end = region_base + snap.view_size;
      char *req_end = static_cast<char *>(addr) + rounded_size;
      const bool inside_region =
          static_cast<char *>(addr) >= region_base && req_end <= region_end;
      const auto shape = snap.region->current_shape();
      if (inside_region &&
          is_fully_committed_shape(shape, snap.region->flags))
        return protect_region(process, addr, rounded_size, new_prot, prot,
                              *snap.region, region_base);
    }
  }

  // ----- Slow path: bulk MBI walk -----------------------------------------
  //
  // Walk regions because NtProtectVirtualMemory cannot span multiple
  // allocation regions in a single call. protect_chunk mutates only the
  // current chunk (Protect change or placeholder replacement), so bulk
  // entries for subsequent regions remain valid.
  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  nt_pal::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
  while (walk.next()) {
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    // Resolve the region for this chunk's allocation base. When the chunk
    // belongs to one of our tracked regions, protect_chunk reads
    // REGION_FLAG_COW from the descriptor; for foreign / image VADs the
    // snapshot misses and protect_chunk falls back to MBI inspection.
    windows::SlotSnapshot chunk_snap;
    const windows::memory::RegionDesc *chunk_region = nullptr;
    if (windows::g_mapping_table.snapshot(walk.entry->AllocationBase,
                                          &chunk_snap))
      chunk_region = chunk_snap.region;

    long rc = protect_chunk(process, walk.chunk, walk.chunk_size, new_prot,
                            prot, *walk.entry, chunk_region);
    if (rc != 0)
      return rc;
  }

  return 0;
}

// ===========================================================================
// Kernel function — implements Linux pkey_mprotect semantics in userspace.
// Combines mprotect with protection key association. Returns 0 on success,
// -errno on failure.
// ===========================================================================

intptr_t pkey_mprotect(void *addr, size_t len, int prot, int pkey) {
  if (LIBC_UNLIKELY(!addr)) {
    if (len > 0)
      return -EINVAL;
    return 0;
  }

  if (len == 0)
    return 0;

  // Apply base protection through the internal mprotect engine (handles region
  // walking, placeholder demand-map, SEC_RESERVE commit, mapping table
  // updates, secure memory retry, I-cache flush).
  intptr_t rc = internal::mprotect(addr, len, prot);
  if (rc != 0)
    return rc;

  // pkey == -1: no key association (plain mprotect behavior).
  if (pkey == -1)
    return 0;

#ifndef LIBC_TARGET_ARCH_IS_X86_64
  (void)pkey;
  return -ENOSYS;
#else
  if (pkey < 0 || pkey >= windows::PKEY_COUNT)
      return -EINVAL;

    uint32_t alloc_bits =
      g_pcb.pkey.allocated.load(cpp::MemoryOrder::RELAXED);
  if (!(alloc_bits & (1u << pkey)))
    return -EINVAL;

  if (!windows::pkey_register_range(addr, len, pkey, prot))
    return -ENOMEM;

  return 0;
#endif
}

} // namespace internal

} // namespace LIBC_NAMESPACE_DECL
