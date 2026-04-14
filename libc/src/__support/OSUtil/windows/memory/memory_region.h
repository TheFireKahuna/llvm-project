//===-- Windows memory region operations -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Placeholder-based memory operations for POSIX mmap emulation on Windows.
//
// POSIX mmap/munmap semantics that have no direct Windows equivalent:
//   - munmap can partially unmap any page-aligned subrange
//   - MAP_FIXED atomically replaces existing mappings
//   - mmap returns addresses whose subranges can be independently unmapped
//
// Windows placeholders (MEM_RESERVE_PLACEHOLDER) bridge the gap:
//   - Split reserved regions at arbitrary page-aligned boundaries
//   - Replace placeholders with committed memory or file views
//   - Convert committed memory back to placeholders for resplitting
//
// All operations use NT APIs directly (NtAllocateVirtualMemoryEx,
// NtCreateSectionEx, NtMapViewOfSectionEx, NtFreeVirtualMemory,
// NtQueryVirtualMemory, NtProtectVirtualMemory) — no Win32 memory APIs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/numa_policy.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Region State Classification
//===----------------------------------------------------------------------===//

/// Region type determines the teardown strategy in prepare_for_fixed.
enum class RegionType {
  Free,           ///< Unallocated address space
  Private,        ///< VirtualAlloc-based private memory
  MappedFile,     ///< File mapping view (data file)
  MappedImage,    ///< Executable image mapping (cannot be replaced)
  MappedPageFile, ///< Pagefile-backed section (e.g., ring buffer)
};

/// Region metadata from NtQueryVirtualMemory(MemoryRegionInformationEx).
/// Must use info class 7 (Ex), not 3 — class 3 returns raw allocation type
/// constants in RegionType instead of the proper bitfield, so fields like
/// PlaceholderReservation are always zero.
struct RegionInfo {
  RegionType type;
  void *alloc_base;     ///< Base of the containing allocation
  SIZE_T region_size;   ///< Size of contiguous region with same attributes
  SIZE_T commit_size;   ///< Committed portion (0 = reserved only)
  ULONG alloc_protect;  ///< Original allocation protection
  bool is_committed;    ///< True if commit_size > 0

  // NT-only extended fields (not in Win32 wrapper).
  bool is_placeholder;
  bool is_large_page;
  bool is_64k_page;
  bool is_enclave;
  bool is_awe;
  bool is_write_watch;
  ULONG_PTR numa_node;
  ULONG_PTR partition_id;
};

/// Returns false for free/unallocated memory or query failure, zeroing *info.
LIBC_INLINE bool query_region(const void *addr, RegionInfo *info) {
  MEMORY_REGION_INFORMATION mri;
  SIZE_T return_size;

  NTSTATUS status = ::NtQueryVirtualMemory(
      NtCurrentProcess(), const_cast<void *>(addr), MemoryRegionInformationEx,
      &mri, sizeof(mri), &return_size);

  if (NT_ERROR(status)) {
    // NtQueryVirtualMemory returns STATUS_INVALID_PARAMETER for free regions.
    if (info) {
      info->type = RegionType::Free;
      info->alloc_base = nullptr;
      info->region_size = 0;
      info->commit_size = 0;
      info->alloc_protect = 0;
      info->is_committed = false;
      info->is_placeholder = false;
      info->is_large_page = false;
      info->is_64k_page = false;
      info->is_enclave = false;
      info->is_awe = false;
      info->is_write_watch = false;
      info->numa_node = 0;
      info->partition_id = 0;
    }
    return false;
  }

  if (info) {
    if (mri.MappedImage)
      info->type = RegionType::MappedImage;
    else if (mri.MappedDataFile || mri.DirectMapped)
      info->type = RegionType::MappedFile;
    else if (mri.MappedPageFile || mri.MappedPhysical)
      info->type = RegionType::MappedPageFile;
    else if (mri.Private)
      info->type = RegionType::Private;
    else
      info->type = RegionType::Free;

    info->alloc_base = mri.AllocationBase;
    info->region_size = mri.RegionSize;
    info->commit_size = mri.CommitSize;
    info->alloc_protect = mri.AllocationProtect;
    info->is_committed = (mri.CommitSize > 0);

    info->is_placeholder = mri.PlaceholderReservation != 0;
    info->is_large_page = (mri.PageSizeLarge != 0) || (mri.PageSizeHuge != 0);
    info->is_64k_page = mri.PageSize64K != 0;
    info->is_enclave = mri.SoftwareEnclave != 0;
    info->is_awe = mri.MappedAwe != 0;
    info->is_write_watch = mri.MappedWriteWatch != 0;
    info->numa_node = mri.NodePreference;
    info->partition_id = mri.PartitionId;
  }

  return true;
}

/// True if addr is unallocated address space.
LIBC_INLINE bool is_free(const void *addr) {
  RegionInfo info;
  return !query_region(addr, &info);
}

/// True if addr is at its allocation base (required for MEM_RELEASE).
LIBC_INLINE bool is_at_alloc_base(const void *addr) {
  RegionInfo info;
  if (!query_region(addr, &info))
    return true; // Free memory has no base to compare against.
  return (addr == info.alloc_base);
}

// Placeholder operations and vm_* primitives are in memory_primitives.h.

//===----------------------------------------------------------------------===//
// MAP_FIXED Support
//===----------------------------------------------------------------------===//



/// Free all existing allocations in [addr, addr+size) so the range can be
/// reused by a new MAP_FIXED allocation.
///
/// Unlike Linux MAP_FIXED (kernel-atomic), this is a multi-step userspace
/// operation. Callers needing atomicity must use external synchronization.
///
/// Cases (target = MAP_FIXED range, X = existing allocation):
///
///   Case 1: Allocation fully contained -- release outright.
///     Target: |============================|
///     Alloc:       |XXXXXXXX|
///     After:  |........free................|
///
///   Case 2: Allocation starts at target base, extends past end.
///     Target: |============|
///     Alloc:  |XXXXXXXXXXXXXXXXXXXXX|
///     After:  |....free....|placeholder...|
///
///   Case 3: Target starts mid-allocation (DATA LOSS -- see above).
///     Before: |DDDDD|XXXXXXXX|DDDDD|        D = data we don't target
///     Target:       |XXXXXXXX|
///     Step 1: |pppppppppppppppppppp|         placeholder (all data lost)
///     Step 2: |ppppp|XXXXXXXX|ppppp|         split off prefix and suffix
///     Step 3: |ppppp|..free..|ppppp|         release target portion
///
///   Case 4: File/pagefile view -- NtUnmapViewOfSectionEx (unmaps entire view).
///   Case 5: Image mapping -- fail (cannot replace).
///

/// Release all non-free regions in [addr, addr+size) to MEM_FREE.
/// Best-effort cleanup — ignores individual release failures.
LIBC_INLINE void release_range_placeholders(void *addr, SIZE_T size) {
  char *cur = static_cast<char *>(addr);
  char *end = cur + size;
  HANDLE process = NtCurrentProcess();
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query_region(cur, mbi))
      break;
    char *region_end =
        static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    if (region_end > end)
      region_end = end;
    if (mbi.State != MEM_FREE) {
      PVOID b = mbi.AllocationBase;
      SIZE_T s = 0;
      ::NtFreeVirtualMemory(process, &b, &s, MEM_RELEASE);
      // Allocation may have extended past region_end — rescan.
      continue;
    }
    cur = region_end;
  }
}

/// Prepare [addr, addr+size) for MAP_FIXED by tearing down all existing
/// allocations and views.
///
/// Pass 1 — Teardown: MEM_PRIVATE regions are converted to placeholders
/// via decommit + preserve_to_placeholder (no MEM_FREE window during
/// teardown). MEM_MAPPED views are unmapped and released to MEM_FREE.
/// MEM_IMAGE regions cause immediate failure.
///
/// Pass 2 — Placeholder assembly (64KB-aligned addr only):
/// Fills MEM_FREE gaps with placeholders. Gaps at non-64KB boundaries
/// (between allocations with non-64KB-aligned sizes) are handled via
/// merge-back: release the preceding placeholder run back to its 64KB
/// start, then create one bigger placeholder spanning the merged range.
/// Finally, coalesces all placeholders into one contiguous range.
///
/// On success with 64KB-aligned addr: the range is a single contiguous
/// placeholder — callers use PlaceholderRange::from_raw() (deterministic).
///
/// On success with non-64KB addr: the range is MEM_FREE — callers use
/// reserve_with_retry() (legacy contract, only for alloc_file_fixed
/// non-aligned path).
LIBC_INLINE bool prepare_for_fixed(void *addr, SIZE_T size) {
  char *start = static_cast<char *>(addr);
  char *end = start + size;
  char *current = start;
  HANDLE process = NtCurrentProcess();

  // ── Pass 1: Teardown ──

  while (current < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query_region(current, mbi))
      return false;

    char *region_end =
        static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    if (region_end > end)
      region_end = end;

    if (mbi.State == MEM_FREE) {
      current = region_end;
      continue;
    }

    switch (mbi.Type) {
    case MEM_PRIVATE: {
      // Release overlapping MEM_PRIVATE to MEM_FREE. Handles both
      // one-shot allocations (no PlaceholderReservation bit) and
      // placeholder-committed allocations uniformly.
      //
      // Full allocation: vm_release (size=0, canonical full release).
      // Partial: interior_release (sub-range → MEM_FREE, surviving
      //   fragments become independent allocations).
      //
      // Pass 2 rebuilds placeholders from the resulting MEM_FREE regions.
      char *ae = find_alloc_end(mbi.AllocationBase);
      char *overlap_end = (ae < end) ? ae : end;
      SIZE_T overlap_size =
          static_cast<SIZE_T>(overlap_end - current);

      if (current == static_cast<char *>(mbi.AllocationBase) &&
          overlap_end >= ae) {
        // Full allocation: standard release.
        if (!vm_release(mbi.AllocationBase))
          return false;
      } else {
        // Partial overlap: interior_release on the sub-range.
        if (!interior_release(current, overlap_size))
          return false;
      }

      current = overlap_end;
      break;
    }

    case MEM_MAPPED: {
      // Extract handles before tearing down the view.
      // If the unmap fails, re-insert so the view stays tracked.
      MappingEntry mentry;
      bool had_entry = g_mapping_table.extract(mbi.AllocationBase, &mentry);

      NTSTATUS unmap_st = ::NtUnmapViewOfSectionEx(
          process, mbi.AllocationBase, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
      if (NT_ERROR(unmap_st)) {
        if (had_entry)
          g_mapping_table.register_mapping_take(
              mbi.AllocationBase, mentry.view_size,
              ViewSpec{mentry.spec.section, mentry.spec.file,
                       mentry.spec.offset, mentry.spec.prot,
                       mentry.spec.flags});
        return false;
      }

      // Unmap succeeded — close extracted handles.
      if (had_entry) {
        if (mentry.spec.file)
          ::NtClose(mentry.spec.file);
        if (mentry.spec.section)
          ::NtClose(mentry.spec.section);
      }

      // Release the placeholder to MEM_FREE. Pass 2 gap-fill will
      // reclaim it as a placeholder.
      PVOID ph = mbi.AllocationBase;
      SIZE_T ph_size = 0;
      ::NtFreeVirtualMemory(process, &ph, &ph_size, MEM_RELEASE);
      // View may have extended beyond current — rescan at same position.
      continue;
    }

    case MEM_IMAGE:
      return false;

    default:
      return false;
    }
  }

  // ── Pass 2: Placeholder assembly ──
  //
  // For 64KB-aligned addr: fill gaps + coalesce → contiguous placeholder.
  // For non-64KB addr: release all placeholders → MEM_FREE.

  if (!is_alloc_aligned(addr)) {
    release_range_placeholders(addr, size);
    return true;
  }

  // Fill MEM_FREE gaps. run_start tracks the 64KB-aligned start of the
  // contiguous placeholder run being built. When a gap starts at a
  // non-64KB boundary (between allocations whose sizes aren't 64KB
  // multiples), we merge back: release [run_start, gap_start) to
  // MEM_FREE, then create one bigger placeholder from run_start through
  // the gap. run_start is always 64KB-aligned because addr is 64KB and
  // allocation bases are 64KB (NT guarantee).
  current = start;
  char *run_start = nullptr;
  while (current < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query_region(current, mbi))
      return false;
    char *region_end =
        static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    if (region_end > end)
      region_end = end;

    if (mbi.State == MEM_FREE) {
      SIZE_T gap_size = static_cast<SIZE_T>(region_end - current);

      if (is_alloc_aligned(current)) {
        // Gap at 64KB boundary — fill directly.
        if (!create_placeholder(current, gap_size))
          return false;
      } else {
        // Non-64KB gap — merge back with the preceding placeholder run.
        // Release [run_start, current) then recreate covering the gap.
        // run_start is 64KB-aligned → create_placeholder succeeds.
        SIZE_T run_len = static_cast<SIZE_T>(current - run_start);
        release_range_placeholders(run_start, run_len);
        SIZE_T merged_size = static_cast<SIZE_T>(region_end - run_start);
        if (!create_placeholder(run_start, merged_size))
          return false;
      }
      if (!run_start)
        run_start = current;
    } else {
      if (!run_start)
        run_start = current;
    }
    current = region_end;
  }

  // Coalesce all adjacent placeholders into one.
  NTSTATUS st = vm_coalesce_placeholders(addr, size);
  if (NT_ERROR(st))
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
// NUMA-Aware Allocation
//===----------------------------------------------------------------------===//


/// Returns the preferred NUMA node for addr, or 0 if unknown.
LIBC_INLINE ULONG_PTR get_numa_node(const void *addr) {
  RegionInfo info;
  if (query_region(addr, &info))
    return info.numa_node;
  return 0;
}

//===----------------------------------------------------------------------===//
// Ring Buffer (Double-Mapped Circular Buffer)
//===----------------------------------------------------------------------===//

/// Create a double-mapped ring buffer: [0, size) and [size, 2*size) alias the
/// same physical pages, enabling contiguous reads across the wrap boundary.
/// Size must be page-aligned.
///
/// Uses NtCreateSectionEx + NtMapViewOfSectionEx for consistency with the
/// file mapping path. The section is pagefile-backed (INVALID_HANDLE_VALUE).
LIBC_INLINE void *create_ring_buffer(SIZE_T size) {
  if (size == 0 || (size & (get_page_size() - 1)) != 0)
    return nullptr;
  if (size > SIZE_MAX / 2)
    return nullptr;

  // Reserve 2x VA as a single placeholder, then split at midpoint.
  PlaceholderRange whole = PlaceholderRange::reserve(2 * size);
  if (!whole)
    return nullptr;

  PlaceholderRange left, right;
  if (!whole.split(size, &left, &right))
    return nullptr; // ~whole releases the 2x reservation.

  // Pagefile-backed section provides the shared physical storage.
  LARGE_INTEGER section_size;
  section_size.QuadPart = static_cast<LONGLONG>(size);

  HANDLE section = nullptr;
  auto sec_oa = internal_oa();
  NTSTATUS status = ::NtCreateSectionEx(
      &section, SECTION_MAP_READ | SECTION_MAP_WRITE, &sec_oa, &section_size,
      PAGE_READWRITE, SEC_COMMIT, nullptr, nullptr, 0);
  if (NT_ERROR(status))
    return nullptr; // ~left + ~right release both placeholders.

  HANDLE current_process = NtCurrentProcess();
  void *result = left.base();

  // Map section into both placeholders. MEM_REPLACE_PLACEHOLDER atomically
  // replaces each placeholder with a view of the same physical pages.
  LARGE_INTEGER offset_zero = {};
  PVOID base1 = left.base();
  SIZE_T view_size1 = size;
  status = ::NtMapViewOfSectionEx(section, current_process, &base1,
                                   &offset_zero, &view_size1,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                   nullptr, 0);
  if (NT_ERROR(status)) {
    ::NtClose(section);
    return nullptr; // ~halves releases both placeholders.
  }
  (void)left.consume(); // Placeholder consumed by the view.

  LARGE_INTEGER offset_zero2 = {};
  PVOID base2 = right.base();
  SIZE_T view_size2 = size;
  status = ::NtMapViewOfSectionEx(section, current_process, &base2,
                                   &offset_zero2, &view_size2,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                   nullptr, 0);
  if (NT_ERROR(status)) {
    ::NtUnmapViewOfSectionEx(current_process, base1, MEM_UNMAP_NONE);
    ::NtClose(section);
    return nullptr; // ~right releases the remaining placeholder.
  }
  (void)right.consume(); // Placeholder consumed by the view.

  // Views hold references to the section object; handle is no longer needed.
  ::NtClose(section);

  return result;
}

/// Destroy a ring buffer created by create_ring_buffer.
LIBC_INLINE void destroy_ring_buffer(void *ring, SIZE_T size) {
  if (!ring)
    return;
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(), ring, MEM_UNMAP_NONE);
  ::NtUnmapViewOfSectionEx(NtCurrentProcess(),
                            static_cast<char *>(ring) + size, MEM_UNMAP_NONE);
}

//===----------------------------------------------------------------------===//
// Advisory VM Information (madvise/posix_madvise shared helper)
//===----------------------------------------------------------------------===//

// apply_vm_info_per_region → for_committed_batched (memory_primitives.h)
// prefetch_committed_regions → prefetch_committed (memory_primitives.h)

// reset_pages → vm_reset (memory_primitives.h)
// dontneed_private_pages → vm_dontneed (memory_primitives.h)

/// Query max page priority across a range via WSEX batched query.
/// Returns the highest priority value (0-7) across all resident pages.
/// Used by MADV_COLD, MADV_PAGEOUT, and wsex_dontneed_section_pages
/// to skip VmPagePriorityInformation syscall when pages are already cold.
LIBC_INLINE ULONG wsex_max_priority(void *addr, SIZE_T size) {
  constexpr int BATCH = 256;
  const SIZE_T PAGE = get_page_size();
  MEMORY_WORKING_SET_EX_INFORMATION batch[BATCH];

  ULONG max_prio = 0;
  char *cursor = static_cast<char *>(addr);
  char *end = cursor + size;

  while (cursor < end) {
    int n = 0;
    for (; n < BATCH && cursor < end; n++, cursor += PAGE) {
      batch[n].VirtualAddress = cursor;
      batch[n].VirtualAttributes.Flags = 0;
    }
    NTSTATUS st = ::NtQueryVirtualMemory(
        NtCurrentProcess(), nullptr, MemoryWorkingSetExInformation, batch,
        static_cast<SIZE_T>(n) * sizeof(batch[0]), nullptr);
    if (NT_ERROR(st))
      return 4; // Assume hot on failure.
    for (int i = 0; i < n; i++) {
      ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
      ULONG prio = static_cast<ULONG>((flags >> 24) & 7);
      if (prio > max_prio)
        max_prio = prio;
    }
  }
  return max_prio;
}

/// WSEX-selective MADV_DONTNEED for section views where MEM_DECOMMIT fails.
/// Derived from DiscardVirtualMemory disassembly but 2x faster: skips MBI
/// validation and NtUnlockVirtualMemory. Zeroes only pages with physical
/// backing (working set or standby list). Conditional priority demotion.
LIBC_INLINE void wsex_dontneed_section_pages(void *addr, SIZE_T size) {
  constexpr int BATCH = 256;
  const SIZE_T PAGE = get_page_size();
  MEMORY_WORKING_SET_EX_INFORMATION batch[BATCH];

  ULONG max_prio = 0;
  char *cursor = static_cast<char *>(addr);
  char *end = cursor + size;

  while (cursor < end) {
    int n = 0;
    char *batch_start = cursor;
    for (; n < BATCH && cursor < end; n++, cursor += PAGE) {
      batch[n].VirtualAddress = cursor;
      batch[n].VirtualAttributes.Flags = 0;
    }

    NTSTATUS st = ::NtQueryVirtualMemory(
        NtCurrentProcess(), nullptr, MemoryWorkingSetExInformation, batch,
        static_cast<SIZE_T>(n) * sizeof(batch[0]), nullptr);
    if (NT_ERROR(st)) {
      // Fallback: blind memset remainder (matches DiscardVirtualMemory).
      __builtin_memset(batch_start, 0,
                       static_cast<size_t>(end - batch_start));
      max_prio = 4;
      break;
    }

    for (int i = 0; i < n; i++) {
      ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
      // Skip pages with no physical backing.
      if (!(flags & 1) && (flags & 0xC00000) != 0x400000)
        continue;
      ULONG prio = static_cast<ULONG>((flags >> 24) & 7);
      if (prio > max_prio)
        max_prio = prio;
      __builtin_memset(batch[i].VirtualAddress, 0, PAGE);
    }
  }

  // MEM_RESET entire range — zeroed pages marked discardable.
  vm_reset(addr, size);

  // Conditional priority demotion — skip if already cold.
  if (max_prio > 1) {
    MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = addr;
    range.NumberOfBytes = size;
    MEMORY_PAGE_PRIORITY_INFORMATION prio = {MEMORY_PRIORITY_VERY_LOW};
    ::NtSetInformationVirtualMemory(NtCurrentProcess(),
                                    VmPagePriorityInformation, 1,
                                    &range, &prio, sizeof(prio));
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_H
