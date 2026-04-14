//===---------- Windows msync engine (kernel function) ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX msync - synchronize memory with physical storage.
//
// Implementation uses NtFlushVirtualMemory to flush dirty pages from
// file-backed mappings (MEM_MAPPED/MEM_IMAGE) to the filesystem cache.
//
// Flag mapping:
//   MS_SYNC       -> NtFlushVirtualMemory (waits for cache write)
//                   + NtFlushBuffersFile for physical durability
//   MS_ASYNC      -> Validates the range (ENOMEM for unmapped regions)
//                   but skips the flush. Kernel lazy writeback handles
//                   dirty mapped pages automatically. Truly non-blocking.
//   MS_INVALIDATE -> Reverts COW pages within MAP_PRIVATE file views to
//                   the original file content via PAGE_REVERT_TO_FILE_MAP.
//                   For MAP_SHARED views, coherency is automatic within a
//                   single process (no action needed).
//
// Durability:
//   NtFlushVirtualMemory flushes dirty pages to the filesystem cache.
//   For MS_SYNC, NtFlushBuffersFile is additionally called via the mapping
//   table (mapping_table.h) to ensure write-through to physical media,
//   satisfying POSIX durability requirements. This works for all file-backed
//   mappings tracked by mmap; untracked mappings flush to cache only.
//
//   MS_ASYNC skips the explicit flush entirely. The kernel's modified page
//   writer flushes dirty mapped pages to disk asynchronously as part of
//   normal memory management. POSIX only requires MS_ASYNC to "initiate"
//   writeback, which the kernel does automatically.
//
//===----------------------------------------------------------------------===//

#include "msync_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h> // UINTPTR_MAX

namespace LIBC_NAMESPACE_DECL {

namespace {

/// Flush an untracked file-backed mapping to physical media by querying
void flush_file_handle(HANDLE fh) {
  IO_STATUS_BLOCK io_status = {};
  ::NtFlushBuffersFile(fh, &io_status);
}

/// the backing file name, opening it, calling NtFlushBuffersFile, and closing.
/// Best-effort: silently returns if any step fails.
void flush_untracked_mapping(HANDLE process, void *addr) {
  // Query the backing file's NT path via MemoryMappedFilenameInformation.
  // Stack buffer for typical paths; longer paths are silently skipped.
  alignas(UNICODE_STRING) char buf[512];
  SIZE_T return_length;
  NTSTATUS status = ::NtQueryVirtualMemory(
      process, addr, MemoryMappedFilenameInformation, buf, sizeof(buf),
      &return_length);
  if (NT_ERROR(status))
    return;

  // The result is a UNICODE_STRING containing the NT device path
  // (e.g., \Device\HarddiskVolume3\path\file).
  auto *name = reinterpret_cast<UNICODE_STRING *>(buf);
  if (!name->Buffer || name->Length == 0)
    return;

  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, name, OBJ_CASE_INSENSITIVE, nullptr,
                             nullptr);
  IO_STATUS_BLOCK io_status;
  HANDLE fh;
  status = ::NtOpenFile(&fh, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &io_status,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
  if (NT_ERROR(status))
    return;

  flush_file_handle(fh);
  ::NtClose(fh);
}

/// Revert COW'd pages within a file-backed MAP_PRIVATE view to the original
/// file content. When a MAP_PRIVATE page is written, Windows creates a
/// private copy (MEM_PRIVATE) backed by the pagefile. PAGE_REVERT_TO_FILE_MAP
/// discards the private copy and restores the page to MEM_MAPPED state,
/// re-faulting from the original file on next access.
///
/// This is the correct MS_INVALIDATE behavior for MAP_PRIVATE: POSIX says
/// subsequent references shall obtain data consistent with permanent storage.
void revert_cow_pages(HANDLE process, char *start, char *end) {
  char *cur = start;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!windows::query_region(cur, mbi))
      break;

    char *region_end = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    char *chunk_end = (region_end < end) ? region_end : end;

    // Look for committed MEM_PRIVATE pages that are COW copies of a file
    // view. These have MEM_PRIVATE type but their AllocationBase matches
    // a tracked file mapping (the original mmap created a MEM_MAPPED view,
    // and writes converted individual pages to MEM_PRIVATE).
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
      // Check if this private region falls within a tracked file mapping.
      windows::SlotSnapshot snap = {};
      bool have_snap =
          windows::g_mapping_table.snapshot(mbi.AllocationBase, &snap);
      if (have_snap && snap.file_handle) {
        // This is a COW'd page within a file view. Revert it.
        PVOID base = cur;
        SIZE_T chunk_size = static_cast<SIZE_T>(chunk_end - cur);
        ULONG old_prot;
        // PAGE_REVERT_TO_FILE_MAP discards the private copy and restores
        // the page to file-backed state. The page protection reverts to
        // the view's original protection.
        ::NtProtectVirtualMemory(process, &base, &chunk_size,
                                 PAGE_REVERT_TO_FILE_MAP, &old_prot);
        // Best-effort: if revert fails (e.g., page was never actually
        // COW'd, or the section was closed), continue with next region.
      }
    }

    cur = chunk_end;
  }
}

} // namespace

namespace internal {

intptr_t msync(void *addr, size_t len, int flags) {
  if (LIBC_UNLIKELY(!addr))
    return -EINVAL;

  if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
    return -EINVAL;

  // POSIX requires exactly one of MS_ASYNC or MS_SYNC, optionally | MS_INVALIDATE.
  const bool has_async = (flags & MS_ASYNC) != 0;
  const bool has_sync = (flags & MS_SYNC) != 0;
  const bool has_invalidate = (flags & MS_INVALIDATE) != 0;

  if (LIBC_UNLIKELY(has_async && has_sync))
    return -EINVAL;

  if (LIBC_UNLIKELY(!has_async && !has_sync))
    return -EINVAL;

  if (LIBC_UNLIKELY((flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) != 0))
    return -EINVAL;

  if (len == 0)
    return 0;

  const SIZE_T rounded_len = windows::round_to_page(len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    return -ENOMEM;

  // Guard against pointer overflow before entering the region loop.
  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(addr_val > UINTPTR_MAX - rounded_len))
    return -ENOMEM;

  HANDLE process = NtCurrentProcess();
  char *cur = static_cast<char *>(addr);
  char *range_end = cur + rounded_len;

  // MS_INVALIDATE: revert COW'd pages in MAP_PRIVATE file views to the
  // original file content. This must happen before the flush so that
  // any subsequent read in the same msync window sees file data.
  if (has_invalidate)
    revert_cow_pages(process, cur, range_end);

  // Validate and flush. Iterates regions because NtFlushVirtualMemory
  // cannot span multiple allocation regions.
  //
  // MS_ASYNC: POSIX only requires initiating writeback; the kernel's lazy
  // writeback already flushes dirty mapped pages to the filesystem cache
  // periodically. We validate the range (ENOMEM for MEM_FREE) but skip
  // the explicit flush for true non-blocking behavior.
  //
  // MS_SYNC: flush dirty pages to cache via NtFlushVirtualMemory, then
  // write-through to physical media via NtFlushBuffersFile.
  // Bulk VA walk — NtFlushVirtualMemory and NtFlushBuffersFile do not alter
  // VA region boundaries, so the bulk snapshot stays valid throughout.
  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  windows::RegionWalker walk(cur, static_cast<SIZE_T>(range_end - cur),
      reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(ws.data()),
      ws.size());

  while (walk.next()) {
    // Linux msync returns ENOMEM for unmapped regions in the range.
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    // MS_SYNC: explicitly flush committed file-backed regions.
    // MEM_MAPPED = file view, MEM_IMAGE = PE image section.
    // Private/anonymous memory has no backing store to flush.
    if (has_sync && walk.entry->State == MEM_COMMIT &&
        (walk.entry->Type == MEM_MAPPED || walk.entry->Type == MEM_IMAGE)) {
      PVOID base = walk.chunk;
      SIZE_T flush_size = walk.chunk_size;
      IO_STATUS_BLOCK io_status = {};
      NTSTATUS flush_status =
          ::NtFlushVirtualMemory(process, &base, &flush_size, &io_status);

      // STATUS_NOT_MAPPED_VIEW can occur if the mapping was torn down
      // between our query and flush. Tolerate it -- the data is gone anyway.
      if (LIBC_UNLIKELY(NT_ERROR(flush_status) &&
                        flush_status != STATUS_NOT_MAPPED_VIEW))
        return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(flush_status));

      // Flush through to physical media. Try the mapping table first;
      // fall back to querying the backing file name and opening it directly
      // (handles untracked mappings from table overflow or external sources).
      if (walk.entry->Type == MEM_MAPPED) {
        windows::SlotSnapshot flush_snap = {};
        if (windows::g_mapping_table.snapshot(walk.entry->AllocationBase,
                                              &flush_snap) &&
            flush_snap.file_handle) {
          flush_file_handle(flush_snap.file_handle);
        } else {
          // Fallback: query the backing file via MemoryMappedFilenameInformation,
          // open it, flush, close. This is expensive but only triggers when the
          // mapping table doesn't have the entry.
          flush_untracked_mapping(process, walk.chunk);
        }
      }
    }

    // MS_ASYNC: no explicit flush -- kernel lazy writeback handles it.
    // We still iterate to validate the range (ENOMEM check above).
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
