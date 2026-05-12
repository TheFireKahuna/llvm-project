//===- msync.cpp - POSIX msync on the read-only nt_pal surface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// POSIX `msync(addr, len, flags)` — flush dirty pages from file-backed
/// mappings to the filesystem cache, and (for `MS_SYNC`) onward to
/// physical media via `NtFlushBuffersFile`.
///
/// `MS_INVALIDATE` performs a CoW-revert pre-pass on the range:
/// `MEM_COMMIT && MEM_PRIVATE` chunks belonging to a tracked file-backed
/// mapping (per `va_tracker::resolve` shape + `region_flag::COW`) get a
/// `PAGE_REVERT_TO_FILE_MAP` `NtProtect`, restoring their underlying
/// file content. Best-effort per chunk.
///
/// `MS_SYNC` walks the range, calling `nt_pal::flush_virtual_memory`
/// per `MEM_COMMIT && (MEM_MAPPED|MEM_IMAGE)` chunk and then
/// `NtFlushBuffersFile` on the section's file handle (via the tracker
/// if present, via `MemoryMappedFilenameInformation` open-by-name as a
/// fallback for untracked mappings — both paths best-effort silent).
///
/// `MS_ASYNC` validates the range (mid-walk `MEM_FREE` → `ENOMEM`) and
/// returns; the kernel's modified-page writer satisfies POSIX's
/// "initiate writeback" obligation asynchronously.
///
/// `STATUS_NOT_MAPPED_VIEW` from `NtFlushVirtualMemory` is the TOCTOU
/// window between our walker snapshot and the per-chunk flush; tolerate
/// it and continue. Promoting it to errno is a regression.
///
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

/// Resolve `addr` against the va_tracker; returns nullptr on miss
/// (untracked VA — foreign mapping, image region, or libc-internal).
/// Caller must already hold `anchor_backing_reader_pin` if it intends
/// to dereference the returned desc's `backing_ref`.
LIBC_INLINE vt::RegionDesc *resolve_desc_or_null(void *addr) {
  auto r = vt::resolve(addr);
  if (!r.has_value())
    return nullptr;
  return r.value().desc;
}

/// Open the section's backing file by name and call `NtFlushBuffersFile`.
/// Best-effort — every failure path returns silently. The exact NT flag
/// set is load-bearing: `FILE_WRITE_DATA | SYNCHRONIZE` for the access
/// mask matches the `NtFlushBuffersFile` requirement; the share mask
/// (`R | W | D`) coexists with the original opener's lock posture; and
/// `FILE_SYNCHRONOUS_IO_NONALERT` keeps the flush blocking but
/// non-alertable so a stray APC cannot abort the syscall.
void flush_untracked_mapping(void *addr) {
  // Roughly UNICODE_STRING + 520 WCHARs covers every practical NT path
  // (long-path NTFS limit is 32 K WCHARs but real-world paths are well
  // under 520).
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

/// MS_INVALIDATE pre-pass: revert CoW'd pages on `MAP_PRIVATE` file
/// views to their underlying file content. `PAGE_REVERT_TO_FILE_MAP`
/// drops the private copy and re-faults from the section on next
/// access. Best-effort per chunk; revert failures are silent because
/// the mapping may simply have nothing to revert (every page already
/// shared).
void revert_cow_pages(char *start, char *end) {
  auto ws = ::LIBC_NAMESPACE::windows::byte_scratch(4096);
  if (!ws)
    return;
  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(
      start, static_cast<SIZE_T>(end - start), ws.data(), ws.size());

  // No backing pin needed here — we only read desc shape + COW flag,
  // which the resolve-side skiplist pin already covers for the
  // duration of the local read window. The next iteration's resolve
  // rotates the pin, but we've finished reading by then.
  while (walk.next()) {
    if (walk.entry->State != MEM_COMMIT || walk.entry->Type != MEM_PRIVATE)
      continue;

    // CoW'd pages live inside a tracked file-backed view. Resolve the
    // chunk's allocation base; only mappings whose desc carries
    // `region_flag::COW` are eligible for revert.
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

  // Unlike mlock (NULL → ENOMEM), msync's NULL-addr path is EINVAL.
  // Linux preserves this distinction; portable apps test the errno to
  // discriminate "argument shape" from "range not mapped".
  if (LIBC_UNLIKELY(addr == nullptr))
    return -EINVAL;
  if (LIBC_UNLIKELY(!mp::is_page_aligned(addr)))
    return -EINVAL;

  // Flag validation runs BEFORE the len==0 short-circuit so that a
  // len-zero call with an illegal flag combination still surfaces
  // EINVAL — Linux semantic preserved by glibc tests.
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

  // MS_INVALIDATE runs before flush — otherwise the flush would commit
  // the CoW copy to the cache that the revert was about to discard.
  if (has_invalidate)
    revert_cow_pages(cur, range_end);

  // Walk the range. Per-region MEM_FREE is ENOMEM for both MS_SYNC and
  // MS_ASYNC; MS_ASYNC stops at validation and skips the flush.
  auto ws = ::LIBC_NAMESPACE::windows::byte_scratch(4096);
  if (!ws)
    return -ENOMEM;
  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(cur, rounded_len, ws.data(),
                                              ws.size());

  // Reader pin for resolve + deref_backing_raw on the MS_SYNC durability
  // path. Anchored once outside the loop; the per-call generation check
  // on `BackingView` (or the fact that the desc is held alive by the
  // resolve pin until the next va_tracker call) covers the recycle race.
  ::LIBC_NAMESPACE::windows::va_tracker::anchor_backing_reader_pin();

  while (walk.next()) {
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;

    // MS_ASYNC: validate-only. The kernel's modified-page writer makes
    // dirty mapped pages durable on its own schedule — POSIX only
    // requires "initiate writeback," which is implicit.
    if (!has_sync)
      continue;

    // MS_SYNC: flush only committed, file-backed (mapped or image)
    // chunks. Anonymous private has no file behind it; placeholder
    // memory has nothing to flush.
    if (walk.entry->State != MEM_COMMIT)
      continue;
    if (walk.entry->Type != MEM_MAPPED && walk.entry->Type != MEM_IMAGE)
      continue;

    PVOID base = walk.chunk;
    SIZE_T sz = walk.chunk_size;
    IO_STATUS_BLOCK flush_iosb = {};
    NTSTATUS flush_st = ::LIBC_NAMESPACE::nt_pal::flush_virtual_memory(
        base, sz, &flush_iosb);

    // STATUS_NOT_MAPPED_VIEW means the mapping went away between the
    // walker snapshot and the flush — TOCTOU. The data the caller
    // wanted flushed is gone with the mapping, so dropping the chunk
    // silently is the correct behaviour.
    if (LIBC_UNLIKELY(NT_ERROR(flush_st) &&
                      flush_st != STATUS_NOT_MAPPED_VIEW))
      return -static_cast<intptr_t>(
          ::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(flush_st));

    // Durability: NtFlushVirtualMemory only writes to the cache.
    // Promote to physical media via NtFlushBuffersFile on the section's
    // backing file. Tracked path resolves through the va_tracker; the
    // open-by-name fallback covers untracked mappings (table miss,
    // foreign mapping, etc.).
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
      // Tracker miss or pagefile-backed section (no file behind it).
      // The fallback discriminates by attempting the open: a pagefile
      // section has no path, so MemoryMappedFilenameInformation fails
      // and the helper returns silently.
      flush_untracked_mapping(walk.chunk);
    }
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
