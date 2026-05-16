//===- msync.cpp - POSIX msync on the read-only nt_pal surface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/msync.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/nt_pal/section.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Caller must hold `anchor_backing_reader_pin` to deref the returned
// desc's `backing_ref` safely; nullptr means tracker miss.
LIBC_INLINE vt::RegionDesc *resolve_desc_or_null(void *addr) {
  auto r = vt::resolve(addr);
  if (!r.has_value())
    return nullptr;
  return r.value().desc;
}

// Untracked-mapping fallback: open the file by mapped name and flush.
// Every failure path returns silently — best-effort. Flag set is
// load-bearing: `FILE_WRITE_DATA | SYNCHRONIZE` is the minimum
// NtFlushBuffersFile accepts; the wide `R|W|D` share mask coexists with
// the original opener's lock posture; `FILE_SYNCHRONOUS_IO_NONALERT`
// blocks the flush non-alertably so a stray APC can't abort mid-write.
void flush_untracked_mapping(void *addr) {
  // 520 WCHARs covers every practical NT path (long-path limit is 32 K
  // but real-world paths stay well under).
  constexpr SIZE_T kBufBytes = sizeof(UNICODE_STRING) + 520 * sizeof(WCHAR);
  auto scratch = ::LIBC_NAMESPACE::windows::byte_scratch(kBufBytes);
  if (!scratch)
    return;

  SIZE_T return_length = 0;
  NTSTATUS st = ::NtQueryVirtualMemory(
      NtCurrentProcess(), addr, MemoryMappedFilenameInformation,
      scratch.data(), scratch.size_bytes(), &return_length);
  if (NT_ERROR(st))
    return;

  auto *name = reinterpret_cast<UNICODE_STRING *>(scratch.data());
  if (name->Buffer == nullptr || name->Length == 0)
    return;

  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, name, OBJ_CASE_INSENSITIVE, nullptr,
                             nullptr);

  IO_STATUS_BLOCK iosb;
  ::LIBC_NAMESPACE::windows::ScopedNtHandle fh;
  st = ::NtOpenFile(fh.put(), FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_SYNCHRONOUS_IO_NONALERT);
  if (NT_ERROR(st))
    return;

  IO_STATUS_BLOCK flush_iosb = {};
  ::NtFlushBuffersFile(fh.get(), &flush_iosb);
}

// MS_INVALIDATE pre-pass: `PAGE_REVERT_TO_FILE_MAP` drops the private
// CoW copy and re-faults from the section — POSIX's "subsequent
// references obtain data consistent with permanent storage" for
// MAP_PRIVATE. Revert failures stay silent (nothing to revert if every
// page is already shared).
void revert_cow_pages(char *start, char *end) {
  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(
      start, static_cast<SIZE_T>(end - start));
  if (!walk)
    return;

  // No backing pin: only desc shape + COW flag are read, both covered
  // by the resolve-side skiplist pin for the local read window.
  while (walk.next()) {
    if (walk.entry->State != MEM_COMMIT || walk.entry->Type != MEM_PRIVATE)
      continue;

    // A CoW'd page is MEM_PRIVATE by current MBI but belongs to a
    // tracked file-backed view; only descs flagged COW are eligible.
    vt::RegionDesc *desc = resolve_desc_or_null(walk.entry->AllocationBase);
    if (desc == nullptr)
      continue;
    if (!desc->is_file_backed() || !desc->is_cow())
      continue;

    PVOID base = walk.chunk;
    SIZE_T sz = walk.chunk_size;
    ULONG old_prot = 0;
    (void)::NtProtectVirtualMemory(NtCurrentProcess(), &base, &sz,
                                    PAGE_REVERT_TO_FILE_MAP, &old_prot);
  }
}

} // namespace

namespace internal {

intptr_t msync(void *addr, size_t len, int flags) {
  namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;

  // EINVAL on NULL (unlike mlock's ENOMEM) — portable apps test the
  // errno to discriminate bad argument shape from unmapped range.
  if (LIBC_UNLIKELY(addr == nullptr))
    return -EINVAL;
  if (LIBC_UNLIKELY(!mp::is_page_aligned(addr)))
    return -EINVAL;

  // Flag validation precedes the len==0 short-circuit so a len-zero
  // call with an illegal flag combination still returns EINVAL —
  // Linux semantic the glibc tests pin.
  if (int e = mp::validate_msync_flags(flags); e != 0)
    return -e;

  if (len == 0)
    return 0;

  const size_t rounded_len = mp::rounded_len_or_zero(len);
  if (LIBC_UNLIKELY(rounded_len == 0))
    return -ENOMEM;
  if (LIBC_UNLIKELY(mp::addr_plus_len_overflows(
          reinterpret_cast<uintptr_t>(addr), rounded_len)))
    return -ENOMEM;

  char *cur = static_cast<char *>(addr);
  char *range_end = cur + rounded_len;

  const bool has_async = (flags & MS_ASYNC) != 0;
  const bool has_invalidate = (flags & MS_INVALIDATE) != 0;
  const bool has_sync = (flags & MS_SYNC) != 0;

  // Invalidate before flush: a flush-first ordering would write the
  // CoW copy to the cache the revert was about to discard.
  if (has_invalidate)
    revert_cow_pages(cur, range_end);

  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(cur, rounded_len);
  if (!walk)
    return -ENOMEM;

  // Reader pin (backing domain, kReaderPin slot) covers every
  // `deref_backing_raw` in the loop below — sibling `resolve` calls
  // rotate a different domain's slot, so this anchor stays valid for
  // the whole walk. The pin alone does not block kill+recycle inside
  // its era; per-load generation re-check on `BackingView` is the
  // structural defence against inter-field staleness.
  ::LIBC_NAMESPACE::windows::va_tracker::anchor_backing_reader_pin();

  while (walk.next()) {
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    // MS_ASYNC degenerates to validate-only: POSIX's "initiate
    // writeback" obligation is satisfied implicitly by the kernel's
    // modified-page writer.
    if (!has_sync)
      continue;

    // Only committed file-backed chunks have anything to flush.
    if (walk.entry->State != MEM_COMMIT)
      continue;
    if (walk.entry->Type != MEM_MAPPED && walk.entry->Type != MEM_IMAGE)
      continue;

    PVOID base = walk.chunk;
    SIZE_T sz = walk.chunk_size;
    IO_STATUS_BLOCK flush_iosb = {};
    NTSTATUS flush_st = ::LIBC_NAMESPACE::nt_pal::flush_virtual_memory(
        base, sz, &flush_iosb);

    // STATUS_NOT_MAPPED_VIEW is the TOCTOU window between the walker
    // snapshot and the per-chunk flush — the mapping (and the data the
    // caller wanted flushed) is gone; silent skip is correct, promoting
    // to errno is a regression.
    if (LIBC_UNLIKELY(NT_ERROR(flush_st) &&
                      flush_st != STATUS_NOT_MAPPED_VIEW))
      return -static_cast<intptr_t>(
          ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(flush_st));

    // NtFlushVirtualMemory only reaches the filesystem cache; MS_SYNC
    // needs physical-media durability, delivered by NtFlushBuffersFile
    // on the section's backing file. MEM_IMAGE is skipped — no POSIX
    // caller contract for it, and reaching its backing PE file would
    // flush unrelated loader state.
    if (walk.entry->Type != MEM_MAPPED)
      continue;

    vt::RegionDesc *desc =
        resolve_desc_or_null(walk.entry->AllocationBase);
    HANDLE file_handle = nullptr;
    if (desc != nullptr) {
      ::LIBC_NAMESPACE::windows::va_tracker::BackingView view(
          ::LIBC_NAMESPACE::windows::va_tracker::deref_backing_raw(
              desc->backing_ref));
      if (view) {
        file_handle = view.load<&::LIBC_NAMESPACE::windows::va_tracker::
                                    DescBacking::file_handle>(
            cpp::MemoryOrder::ACQUIRE);
      }
    }

    if (file_handle != nullptr) {
      IO_STATUS_BLOCK file_iosb = {};
      ::NtFlushBuffersFile(file_handle, &file_iosb);
    } else {
      // Tracker miss or pagefile-backed section. The fallback
      // self-discriminates: pagefile sections have no path, so the
      // MemoryMappedFilenameInformation query fails and the helper
      // returns silently.
      flush_untracked_mapping(walk.chunk);
    }
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
