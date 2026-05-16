//===-- OFD section-handle cache for mmap ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `ofd_acquire_section_for_mmap` returns an owned NT section handle for the
// given disk-kind OFD, using `ofd->disk().section_handle` as a per-fd cache.
// Multiple views of the same section share physical pages — required for
// MAP_SHARED coherence and pagefile-backed-anon double-mapping. Hides the
// dup-on-cache-hit + CAS publish + lose-race close + SEC_64K_PAGES fallback
// from POSIX-layer callers; the only NT-side surface the POSIX layer touches
// is `va_tracker::acquire` with the returned handle in `AcquireMeta`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_SECTION_CACHE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_SECTION_CACHE_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription; // Defined in fd_table.h.

namespace fd {

// Acquires an owned section HANDLE for an mmap of `ofd`.
//
// Protocol:
//   1. ACQUIRE-load `ofd->disk().section_handle`; on hit, NtDuplicateObject
//      into a caller-owned dup and return it.
//   2. On miss, `nt_pal::create_section_file(file_handle, 0, page_prot,
//      section_access, &section, sec_flags)`. If `sec_flags` included
//      `SEC_64K_PAGES` and the kernel rejected (older builds or a file
//      type that disallows 64 K pages), retry once with `SEC_64K_PAGES`
//      stripped — every other flag preserved.
//   3. Re-ACQUIRE-load the cache; if still null, NtDuplicateObject the
//      created section into a cache-side dup and CAS-publish (REL on win,
//      RELAXED on observe). On lose-race, close the dup — the winner's
//      handle is already in the cache and the caller keeps the original.
//
// `ofd` must be a disk-kind OFD with a valid `file_handle`; `section_access`
// must already carry SECTION_QUERY + SECTION_EXTEND_SIZE per the P4
// section-create contract (every file-backed section needs EXTEND_SIZE so
// future ftruncate-driven growth via NtExtendSection succeeds; the right
// cannot be added retroactively to a live section).
//
// On failure returns the kernel-derived POSIX errno (typically EACCES on
// access-mask mismatch, ENOMEM on STATUS_NO_MEMORY, EFAULT on
// unclassifiable NTSTATUS); `ofd->disk().section_handle` is left untouched.
[[nodiscard]] ::LIBC_NAMESPACE::ErrorOr<HANDLE>
ofd_acquire_section_for_mmap(OpenFileDescription *ofd, HANDLE file_handle,
                              ULONG section_access, DWORD page_prot,
                              ULONG sec_flags);

} // namespace fd
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_SECTION_CACHE_H
