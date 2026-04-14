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
//     Partial committed: interior_release (MEM_RELEASE with non-zero size)
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
// that may span multiple regions. This implementation walks regions via
// NtQueryVirtualMemory and applies protection changes per-region.
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
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/remap_transaction.h"
#include "src/__support/OSUtil/windows/memory/memory_region.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/numa_policy.h"
#include "src/__support/OSUtil/windows/memory/region_snapshot.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
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
/// Full release: NtFreeVirtualMemory(MEM_RELEASE) with size=0.
/// Partial committed: decommit target range + punch to placeholder + release.
///   Surrounding committed data survives — only the target is destroyed.
/// Partial placeholder: split at page boundaries and release target.
///
/// Returns 0 on success, -errno on failure.
long unmap_private_chunk(HANDLE process, char *chunk_start,
                                     char *chunk_end, void *alloc_base,
                                     bool is_committed) {
  MEMORY_BASIC_INFORMATION end_mbi;
  bool end_beyond_alloc =
      !windows::query_region(chunk_end, end_mbi) ||
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
    // Committed private memory: interior_release punches the sub-range
    // directly to MEM_FREE in a single syscall. VA is returned to the
    // system; surviving fragments become independent allocations.
    // Replaces the three-step decommit+preserve+coalesce (3-7 syscalls).
    if (!windows::interior_release(chunk_start, target_size))
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
    if (!windows::split_placeholder(alloc_base, target_start - ab))
      return -EINVAL;
  }

  char *alloc_end = windows::find_alloc_end(alloc_base);
  uintptr_t ae = reinterpret_cast<uintptr_t>(alloc_end);
  if (target_end > ae || target_end == 0)
    target_end = ae;

  if (target_end < ae) {
    char *split_base = reinterpret_cast<char *>(target_start);
    SIZE_T split_off = target_end - target_start;
    if (!windows::split_placeholder(split_base, split_off))
      return -EINVAL;
  }

  // Coalesce with neighbors, release if isolated.
  windows::coalesce_or_release_placeholder(
      reinterpret_cast<void *>(target_start), target_end - target_start);
  return 0;
}

//==========================================================================
// File view full unmap
//==========================================================================

/// Unmap an entire file view and release its address space.
///
/// Uses extract (not remove) so handles survive until the NT unmap
/// succeeds. On failure the entry is re-inserted and the view is
/// untouched — no orphaned mappings.
///
/// Returns 0 on success, -errno on failure.
long full_unmap_view(HANDLE process, char *view_base) {
  windows::MappingEntry entry;
  if (!windows::g_mapping_table.extract(view_base, &entry)) {
    // No table entry — still try the unmap (best-effort).
    NTSTATUS st = ::NtUnmapViewOfSectionEx(process, view_base,
                                            MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
    if (NT_ERROR(st))
      return windows_util::ntstatus_to_kerr(st);
    // Coalesce with neighbors and release if isolated.
    MEMORY_BASIC_INFORMATION mbi;
    if (windows::query_region(view_base, mbi))
      windows::coalesce_or_release_placeholder(view_base, mbi.RegionSize);
    else
      windows::release_placeholder(view_base); // Fallback.
    return 0;
  }

  SIZE_T view_size = entry.view_size;

  NTSTATUS st = ::NtUnmapViewOfSectionEx(process, view_base,
                                          MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
  if (NT_ERROR(st)) {
    // NT unmap failed — re-insert the entry so the view stays tracked.
    windows::g_mapping_table.register_mapping_take(
        view_base, entry.view_size,
        windows::ViewSpec{entry.spec.section, entry.spec.file,
                          entry.spec.offset, entry.spec.prot,
                          entry.spec.flags});
    return windows_util::ntstatus_to_kerr(st);
  }

  // Success — close the extracted handles.
  if (entry.spec.file)
    ::NtClose(entry.spec.file);
  if (entry.spec.section)
    ::NtClose(entry.spec.section);

  // Coalesce with adjacent freed placeholders, release if isolated.
  windows::coalesce_or_release_placeholder(view_base, view_size);
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

/// Partially unmap a file view using the split-remap algorithm.
///
/// unmap_start/unmap_end are page-aligned addresses within [view_base, view_end).
/// Partial unmap of a section view via transactional split-remap.
///
/// Uses RemapTransaction to carve the unmap range out of the containing
/// view, remap the kept left/right fragments, then coalesce/release the
/// freed placeholder. On failure, the transaction destructor rolls back.
///
/// Returns 0 on success, -errno on failure.
long partial_unmap_view(HANDLE /*process*/, char *view_base,
                                    char *view_end, uintptr_t unmap_start,
                                    uintptr_t unmap_end) {
  uintptr_t vb = reinterpret_cast<uintptr_t>(view_base);
  uintptr_t ve = reinterpret_cast<uintptr_t>(view_end);

  windows::RemapTransaction txn(view_base, view_end, unmap_start,
                                unmap_end - unmap_start);

  if (!txn.prepare())
    return -EINVAL;

  if (!txn.entry().spec.section) {
    // Large-page views can't be partially unmapped — indivisible.
    // ~txn will call abort_remap (or discard if no section).
    return -EINVAL;
  }

  auto result = txn.execute();
  if (!result.target)
    return -ENOMEM; // ~txn rolls back.

  // Coalesce freed placeholder with adjacent placeholders, then release
  // if isolated. Prevents sub-64KB unrepairable VA gaps.
  auto [freed_base, freed_size] = result.target.consume();
  windows::coalesce_or_release_placeholder(freed_base, freed_size);

  // Coalesce placeholders from failed fragment remaps.
  if (txn.has_left() && !result.left_ok) {
    SIZE_T left_size = unmap_start - vb;
    windows::coalesce_or_release_placeholder(view_base, left_size);
  }
  if (txn.has_right() && !result.right_ok) {
    SIZE_T right_size = ve - unmap_end;
    windows::coalesce_or_release_placeholder(
        reinterpret_cast<void *>(unmap_end), right_size);
  }

  txn.commit();

  if (LIBC_UNLIKELY(!result.left_ok || !result.right_ok))
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
long demand_map_placeholder(HANDLE process, void *addr, SIZE_T size,
                                        DWORD prot) {
  PVOID base = addr;
  SIZE_T sz = size;

  int node = windows::select_numa_node();
  if (node >= 0) {
    MEM_EXTENDED_PARAMETER param = {};
    param.Type = MemExtendedParameterNumaNode;
    param.ULong = static_cast<ULONG>(node);
    NTSTATUS st = ::NtAllocateVirtualMemoryEx(
        process, &base, &sz,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot, &param, 1);
    if (NT_SUCCESS(st))
      return 0;
    // NUMA hint failed — fall through to non-NUMA commit.
    base = addr;
    sz = size;
  }

  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      process, &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  return 0;
}

/// Apply protection to a single region chunk, dispatching on state.
/// Returns 0 on success, -errno on failure.
long protect_chunk(HANDLE process, void *addr, SIZE_T size,
                               DWORD new_prot, int prot,
                               const MEMORY_BASIC_INFORMATION &mbi) {
  const bool is_committed = (mbi.State == MEM_COMMIT);

  if (!is_committed && prot != PROT_NONE) {
    // SEC_RESERVE section view (MAP_NORESERVE): uncommitted pages within a
    // mapped section. Commit with the requested protection — no need to
    // create a new section, one already backs this view.
    if (mbi.Type == MEM_MAPPED) {
      PVOID base = addr;
      SIZE_T commit_size = size;
      NTSTATUS st = ::NtAllocateVirtualMemoryEx(process, &base, &commit_size,
                                                 MEM_COMMIT, new_prot,
                                                 nullptr, 0);
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
  // Failure codes specific to protection changes:
  //   STATUS_SECTION_PROTECTION    → EACCES (prot exceeds section access mask)
  //   STATUS_DYNAMIC_CODE_BLOCKED  → EACCES (ACG active; W^X policy violation)
  // Both are mapped in ntstatus_to_errno.
  PVOID base = addr;
  SIZE_T region_size = size;
  ULONG old_protect;
  NTSTATUS status = ::NtProtectVirtualMemory(process, &base, &region_size,
                                             new_prot, &old_protect);
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
    MEMORY_BASIC_INFORMATION mbi;
    bool query_ok = windows::query_region(current, mbi);

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
      char *view_end = windows::find_alloc_end(view_base);

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

  // Walk regions because NtProtectVirtualMemory cannot span multiple
  // allocation regions in a single call. protect_chunk mutates only the
  // current chunk (Protect change or placeholder replacement), so bulk
  // entries for subsequent regions remain valid.
  HANDLE process = NtCurrentProcess();

  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  windows::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
  while (walk.next()) {
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    long rc = protect_chunk(process, walk.chunk, walk.chunk_size, new_prot,
                            prot, *walk.entry);
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
