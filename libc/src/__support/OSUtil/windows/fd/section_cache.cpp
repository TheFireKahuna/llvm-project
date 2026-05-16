//===-- OFD section-handle cache for mmap ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fd/section_cache.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt_pal/section.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/libc_assert.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace fd {

::LIBC_NAMESPACE::ErrorOr<HANDLE>
ofd_acquire_section_for_mmap(OpenFileDescription *ofd, HANDLE file_handle,
                              ULONG section_access, DWORD page_prot,
                              ULONG sec_flags) {
  LIBC_ASSERT(ofd != nullptr);
  LIBC_ASSERT(ofd->kind == FileKind::Disk);

  HANDLE process = NtCurrentProcess();

  // Cache hit: dup so caller owns an independent handle. The cache
  // retains the master; callers always receive an owned dup so the
  // RegionPool / va_tracker can take ownership without aliasing the
  // cache (which would dangle on unmap).
  HANDLE cached =
      ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (cached != nullptr) {
    HANDLE dup = nullptr;
    NTSTATUS st = ::NtDuplicateObject(process, cached, process, &dup, 0, 0,
                                       DUPLICATE_SAME_ACCESS);
    if (NT_SUCCESS(st))
      return dup;
    return windows_util::nt_error(st);
  }

  // Cache miss: create. Try with caller-supplied `sec_flags` first; on
  // kernel rejection, retry once with `SEC_64K_PAGES` stripped — older
  // builds and some file types reject the 64 K-page request. Every
  // other flag bit is preserved verbatim.
  HANDLE section = nullptr;
  NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::create_section_file(
      file_handle, /*max_size=*/0, page_prot, section_access, &section,
      sec_flags);
  if (NT_ERROR(st) && (sec_flags & SEC_64K_PAGES) != 0) {
    const ULONG fallback_flags = sec_flags & ~SEC_64K_PAGES;
    st = ::LIBC_NAMESPACE::nt_pal::create_section_file(
        file_handle, /*max_size=*/0, page_prot, section_access, &section,
        fallback_flags);
  }
  if (NT_ERROR(st))
    return windows_util::nt_error(st);

  // Publish a dup into the cache so subsequent mmap calls on this fd
  // skip NtCreateSectionEx. Pre-check before dup-and-CAS to avoid the
  // dup+close syscall pair when a peer already published — kernel
  // serialises NtCreateSectionEx on the SectionObjectPointers lock so
  // losers usually arrive after the winner has stored.
  if (ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE) == nullptr) {
    HANDLE cache_dup = nullptr;
    NTSTATUS dup_st = ::NtDuplicateObject(process, section, process,
                                           &cache_dup, 0, 0,
                                           DUPLICATE_SAME_ACCESS);
    if (NT_SUCCESS(dup_st)) {
      HANDLE expected = nullptr;
      if (!ofd->disk().section_handle.compare_exchange_strong(
              expected, cache_dup, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::RELAXED)) {
        // Lost race — winner's handle already stored. Drop the dup.
        ::LIBC_NAMESPACE::nt_pal::close_section(cache_dup);
      }
    }
    // Dup failure on the cache side is non-fatal: the caller still
    // gets the created section; next mmap on this fd just re-creates.
  }

  return section;
}

} // namespace fd
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
