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
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/remap_guard.h"
#include "src/__support/OSUtil/windows/memory/memory_region.h"
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
  if (LIBC_UNLIKELY(!windows::query_region(addr, mbi) ||
                    mbi.Type != MEM_MAPPED))
    return -EINVAL;

  void *view_base = mbi.AllocationBase;
  SIZE_T view_size = mbi.RegionSize;

  // Use RemapGuard for RAII rollback.
  windows::RemapGuard guard(view_base, view_size);
  if (!guard.prepare() || !guard.entry().spec.section)
    return -EINVAL;

  // Compute the new file offset from pgoff (in pages).
  const SIZE_T page_size = windows::get_page_size();
  windows::ViewSpec new_spec = guard.entry().spec;
  new_spec.offset.QuadPart = static_cast<LONGLONG>(pgoff) *
                             static_cast<LONGLONG>(page_size);

  if (!guard.unmap())
    return -ENOMEM; // ~guard remaps original view.

  // Query the placeholder to find its actual size.
  if (!windows::query_region(addr, mbi)) {
    guard.discard();
    return -EINVAL;
  }

  SIZE_T placeholder_size = mbi.RegionSize;

  if (LIBC_UNLIKELY(rounded_size > placeholder_size))
    return -EINVAL; // ~guard remaps original view.

  // Split if the placeholder is larger than the target.
  bool has_tail = (placeholder_size > rounded_size);
  if (has_tail) {
    if (!windows::split_placeholder(addr, rounded_size))
      return -ENOMEM; // ~guard remaps original view.
  }

  // Remap the target range with the new offset.
  NTSTATUS status = new_spec.map_into(addr, rounded_size);
  if (NT_ERROR(status))
    return -static_cast<intptr_t>(
        windows_util::ntstatus_to_errno(status)); // ~guard rolls back.

  // Remap the tail with the original offset to preserve existing data.
  if (has_tail) {
    char *tail_addr = static_cast<char *>(addr) + rounded_size;
    SIZE_T tail_size = placeholder_size - rounded_size;
    windows::ViewSpec tail_spec = guard.entry().spec.at_offset(rounded_size);

    NTSTATUS tail_st = tail_spec.map_into(tail_addr, tail_size);
    if (NT_ERROR(tail_st))
      return -static_cast<intptr_t>(
          windows_util::ntstatus_to_errno(tail_st)); // ~guard rolls back.

    // Register the tail as a separate mapping table entry.
    //
    // INVARIANT: No error path between register_mapping and commit.
    // register_mapping creates a LIVE entry that ~RemapGuard cannot undo.
    // If register fails, we return -ENOMEM and ~guard cleanly rolls back
    // (unmaps noted ranges, coalesces, re-remaps original view). If it
    // succeeds, commit() is the very next call — it is a non-failing
    // store. Do not add fallible operations between these two lines.
    if (!windows::g_mapping_table.register_mapping(
            tail_addr, tail_size, tail_spec))
      return -ENOMEM; // ~guard rolls back (tail unmap + re-remap original).
  }

  // Commit: same base, same handles, updated section_offset.
  guard.commit(view_base, rounded_size, new_spec);
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
