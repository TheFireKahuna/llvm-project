//===---------- Windows remap_file_pages engine (kernel function) ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// remap_file_pages creates non-linear mappings within a file-backed region.
// Deprecated since Linux 3.16 (the kernel now emulates it by creating new
// VMAs), but still supported for compatibility.
//
// Implementation via Windows placeholders:
//   1. begin_remap to lock the mapping entry (REMAPPING state)
//   2. Unmap the target range, preserving its address as a placeholder
//   3. Split the placeholder if the view was larger than the target
//   4. Remap the target with the new file offset (pgoff)
//   5. Remap the tail (if any) with the original offset to preserve data
//   6. commit_remap to finalize (REMAPPING -> LIVE)
//
// The transactional remap API keeps handles alive in the mapping table
// slot throughout the operation — no handle duplication or use-after-close.
//
// Limitations:
//   - addr must be page-aligned and within an existing file-backed mapping
//   - The mapping table must track the section handle (mmap does this)
//   - prot parameter is ignored per Linux behavior (uses existing protection)
//
//===----------------------------------------------------------------------===//

#include "remap_file_pages_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/remap_guard.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/memory/legacy/view_spec.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t remap_file_pages(void *addr, size_t size, int prot, size_t pgoff,
                      int flags) {
  (void)prot;  // Ignored per Linux behavior.
  (void)flags; // Reserved, must be 0 on Linux.

  if (LIBC_UNLIKELY(!addr || !windows::is_page_aligned(addr)))
    return -EINVAL;

  if (LIBC_UNLIKELY(size == 0))
    return -EINVAL;

  const SIZE_T rounded_size = windows::round_to_page(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -EINVAL;

  MEMORY_BASIC_INFORMATION mbi;
  if (LIBC_UNLIKELY(!nt_pal::query_region(addr, mbi) ||
                    mbi.Type != MEM_MAPPED))
    return -EINVAL;

  void *view_base = mbi.AllocationBase;
  SIZE_T view_size = mbi.RegionSize;

  // Use RemapGuard for RAII rollback.
  windows::RemapGuard guard(view_base, view_size);
  if (!guard.prepare() || guard.section_handle() == nullptr)
    return -EINVAL;

  const auto &entry = guard.entry();
  windows::memory::RegionDesc *region = guard.region();

  // Build a transient ViewSpec for the section-borrow remap helpers.
  // The new offset comes from pgoff (in pages); the tail re-uses the
  // region's original offset (preserving the file content past the
  // remapped target range).
  const SIZE_T page_size = windows::get_page_size();
  windows::ViewSpec new_spec{};
  new_spec.section = region->section_handle;
  new_spec.file = region->file_handle;
  new_spec.offset.QuadPart = static_cast<LONGLONG>(pgoff) *
                             static_cast<LONGLONG>(page_size);
  new_spec.prot = entry.view_prot;
  new_spec.flags = entry.flags;

  if (!guard.unmap())
    return -ENOMEM; // ~guard remaps original view.

  // Query the placeholder to find its actual size.
  if (!nt_pal::query_region(addr, mbi)) {
    guard.discard();
    return -EINVAL;
  }

  SIZE_T placeholder_size = mbi.RegionSize;

  if (LIBC_UNLIKELY(rounded_size > placeholder_size))
    return -EINVAL; // ~guard remaps original view.

  // Split if the placeholder is larger than the target.
  bool has_tail = (placeholder_size > rounded_size);
  if (has_tail) {
    if (!nt_pal::split_placeholder(addr, rounded_size))
      return -ENOMEM; // ~guard remaps original view.
  }

  // Remap the target range with the new offset.
  NTSTATUS status = new_spec.map_into(addr, rounded_size);
  if (NT_ERROR(status))
    return -static_cast<intptr_t>(
        windows_util::ntstatus_to_errno(status)); // ~guard rolls back.

  // Remap the tail with the original section offset to preserve existing
  // data. The tail becomes a separate slot pointing at the same region —
  // refcount is bumped by add_ref before publishing.
  if (has_tail) {
    char *tail_addr = static_cast<char *>(addr) + rounded_size;
    SIZE_T tail_size = placeholder_size - rounded_size;
    windows::ViewSpec tail_spec{};
    tail_spec.section = region->section_handle;
    tail_spec.file = region->file_handle;
    tail_spec.offset = region->section_offset;
    tail_spec.offset.QuadPart += static_cast<LONGLONG>(rounded_size);
    tail_spec.prot = entry.view_prot;
    tail_spec.flags = entry.flags;

    NTSTATUS tail_st = tail_spec.map_into(tail_addr, tail_size);
    if (NT_ERROR(tail_st))
      return -static_cast<intptr_t>(
          windows_util::ntstatus_to_errno(tail_st)); // ~guard rolls back.

    // Register the tail as a separate LIVE mapping. add_ref reserves the
    // refcount this slot will own; on register failure release it and
    // unmap so ~guard can roll back cleanly.
    if (entry.region_id != windows::memory::RegionPool::NONE)
      windows::memory::g_region_pool.add_ref(entry.region_id);
    if (!windows::g_mapping_table.register_mapping(
            tail_addr, tail_size, entry.region_id, entry.alloc_id,
            tail_spec.prot, tail_spec.flags)) {
      nt_pal::unmap_view_preserve(tail_addr);
      nt_pal::free_placeholder(tail_addr);
      if (entry.region_id != windows::memory::RegionPool::NONE)
        windows::memory::g_region_pool.release(entry.region_id);
      return -ENOMEM; // ~guard rolls back.
    }
  }

  // Commit: same base, same region, the new offset is captured by the
  // already-mapped view at the section level.
  if (!guard.commit(view_base, rounded_size, entry.region_id, entry.alloc_id,
                    entry.view_prot, entry.flags))
    return -ENOMEM; // ~guard rolls back.
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
