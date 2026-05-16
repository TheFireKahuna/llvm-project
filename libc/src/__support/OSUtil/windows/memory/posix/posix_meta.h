//===- posix_meta.h - AcquireMeta builders by POSIX intent -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `va_tracker::AcquireMeta` builders, one per POSIX call-shape. Each builder
// takes the smallest set of inputs that distinguishes its shape, translates
// POSIX-side flag bits into `region_flag::*` bits, and populates the fields
// `va_tracker::acquire` copies into the freshly-allocated `RegionDesc`.
//
// These builders are the only place where POSIX flag bits become
// substrate-side `region_flag::*` bits. Concentrating the translation at one
// site per shape keeps the SHARED / NORESERVE / HUGE_PAGES / LOW_32BIT / COW
// / COMMITTED bit choices auditable; a misread here would silently propagate
// through fault dispatch, fork classification, and dump exclusion downstream
// of the tracker.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_META_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_META_H

#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/windows/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt_pal/large_pages.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory_posix {

//===----------------------------------------------------------------------===//
// VaRange builder.
//===----------------------------------------------------------------------===//

// Build a `va_tracker::VaRange` from a POSIX `(addr, len)` pair. `len` must
// already be page-rounded — `posix_validation.h::rounded_len_or_zero` is the
// canonical producer.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::VaRange
make_range(void *addr, size_t bytes) {
  return ::LIBC_NAMESPACE::windows::va_tracker::VaRange{addr, bytes};
}

//===----------------------------------------------------------------------===//
// Flag-bit translation helpers.
//===----------------------------------------------------------------------===//

// Translate the POSIX-side flag bits that influence substrate dispatch into
// the corresponding `region_flag::*` mask. Per-shape builders OR in
// shape-fixed bits afterwards (COW on private file views, COMMITTED on
// non-NORESERVE anon, etc.) so this helper carries only the bits whose
// presence is purely caller-driven.
[[nodiscard]] LIBC_INLINE uint16_t common_flag_bits(int posix_flags) {
  uint16_t bits = 0;
  if (posix_flags & MAP_SHARED)
    bits |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::SHARED;
  if (posix_flags & MAP_NORESERVE)
    bits |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::NORESERVE;
  if (posix_flags & MAP_HUGETLB)
    bits |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::HUGE_PAGES;
  if (posix_flags & MAP_32BIT)
    bits |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::LOW_32BIT;
  return bits;
}

//===----------------------------------------------------------------------===//
// Per-intent AcquireMeta builders.
//===----------------------------------------------------------------------===//

// `mmap(MAP_ANONYMOUS | MAP_PRIVATE, ...)` — the canonical anonymous private
// path. No section or file handle. `placeholder_base`/`placeholder_size`
// stay at default zero so the tracker treats the registered range as the
// placeholder identity (`va_tracker::AcquireMeta`: "Both zero means
// 'identity equals range'"). `COMMITTED` is forced unless the caller passed
// `MAP_NORESERVE` — POSIX private-anon defaults to backing store reserved
// at acquire time.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
anon_private_meta(int prot, int posix_flags) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  if (!(posix_flags & MAP_NORESERVE))
    m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  return m;
}

// `mmap(MAP_ANONYMOUS | MAP_SHARED, ...)` — pagefile-backed section view.
// Caller supplies the section handle from `nt_pal::create_section_anon`.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
anon_shared_meta(int prot, int posix_flags, HANDLE section) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::SHARED;
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.section_handle = section;
  return m;
}

// `mmap(fd, MAP_PRIVATE, ...)` — CoW file view. `view_prot` uses the
// WRITECOPY translation so writes trigger kernel-side CoW without requiring
// the section to be opened for write access — the same hardening choice the
// validation layer documents on `prot_to_page_flags_cow`.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
file_private_meta(int prot, int posix_flags, HANDLE section, HANDLE file,
                  uint64_t section_offset) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page_cow(prot);
  m.flags = common_flag_bits(posix_flags);
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COW;
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.section_handle = section;
  m.file_handle = file;
  m.section_offset = section_offset;
  return m;
}

// `mmap(fd, MAP_SHARED, ...)` — write-through file view.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
file_shared_meta(int prot, int posix_flags, HANDLE section, HANDLE file,
                 uint64_t section_offset) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::SHARED;
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.section_handle = section;
  m.file_handle = file;
  m.section_offset = section_offset;
  return m;
}

// `mmap(... | MAP_HUGETLB, ...)` — large-page anonymous section. `kind` is
// accepted for signature symmetry with the validation/decoding layer
// (`posix_validation.h::decode_map_huge_shift`); the page size is already
// fixed inside the section handle by `nt_pal::commit_replace_large`, so
// this builder has no kind-specific field to set.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
hugetlb_meta(int prot, int posix_flags,
             ::LIBC_NAMESPACE::nt_pal::LargePageKind /*kind*/,
             HANDLE section) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::HUGE_PAGES;
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  if (posix_flags & MAP_SHARED)
    m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::SHARED;
  m.section_handle = section;
  return m;
}

// `brk` — single-shot anonymous reservation. The placeholder identity covers
// the full kernel reservation while the registered range is the narrow
// `[base, cursor)` brk window. `placeholder_base` / `placeholder_size` are
// populated explicitly so the substrate retains the reservation across
// cursor extensions without re-acquiring VA on each `sbrk` grow.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
brk_meta(int prot, void *placeholder_base, size_t placeholder_size) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.placeholder_base = placeholder_base;
  m.placeholder_size = placeholder_size;
  return m;
}

// `shm_open` followed by `mmap` — POSIX shared-memory view. Caller supplies
// the named section handle from `nt_pal::create_section_named`.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
shm_posix_meta(int prot, int posix_flags, HANDLE section) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::SHARED;
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.section_handle = section;
  return m;
}

// `shmat` — SysV shm view. The substrate metadata is identical to
// `shm_posix_meta`; the two builders stay separate because the SysV segment
// registry consulted at attach time lives at the POSIX layer above the
// tracker, and a future ABI change to one shape (e.g. SHM_HUGETLB lifecycle)
// must not silently affect the other.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
shm_sysv_meta(int prot, int posix_flags, HANDLE section) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::SHARED;
  m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.section_handle = section;
  return m;
}

} // namespace memory_posix
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_META_H
