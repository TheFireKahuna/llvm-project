//===- posix_meta.h - AcquireMeta builders by POSIX intent -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `va_tracker::AcquireMeta` builders, one per POSIX call-shape. Each
/// builder takes the smallest set of inputs that distinguishes its
/// shape, translates POSIX-side flag bits into `region_flag::*` bits,
/// and populates the substrate-bound fields the tracker copies into a
/// fresh `RegionDesc` plus a fresh `DescBacking`.
///
/// The builders are the only place where POSIX flag bits cross into
/// substrate flag bits. A POSIX flag misread here propagates through
/// fault dispatch (NORESERVE, NUMA_INTERLEAVE, PROT_GUARD), fork
/// classification (DONTFORK, WIPEONFORK), and dump exclusion
/// (DUMP_EXCLUDE) — the §6bis preservation contracts depend on this
/// translation being exact at one site per shape.
///
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

/// Build a `va_tracker::VaRange` from a POSIX `(addr, len)` pair. The
/// caller is responsible for page-rounding `len` before this call —
/// `posix_validation.h::rounded_len_or_zero` produces the right value.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::VaRange
make_range(void *addr, size_t bytes) {
  return ::LIBC_NAMESPACE::windows::va_tracker::VaRange{addr, bytes};
}

//===----------------------------------------------------------------------===//
// Flag-bit translation helpers.
//===----------------------------------------------------------------------===//

/// Translate the POSIX flag bits that influence substrate-side dispatch
/// into the corresponding `region_flag::*` mask. Per-shape builders
/// then OR in any shape-fixed bits (e.g. `COW` on private file views).
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

/// `mmap(MAP_ANONYMOUS | MAP_PRIVATE, ...)` — the canonical anonymous
/// private path. No section handle, no file handle; the tracker takes
/// the placeholder identity from the registered range.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
anon_private_meta(int prot, int posix_flags) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = common_flag_bits(posix_flags);
  if (!(posix_flags & MAP_NORESERVE))
    m.flags |= ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  return m;
}

/// `mmap(MAP_ANONYMOUS | MAP_SHARED, ...)` — pagefile-backed section
/// view. Caller supplies the section handle from
/// `nt_pal::create_section_anon`.
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

/// `mmap(fd, MAP_PRIVATE, ...)` — CoW file view. Caller supplies the
/// section and file handles plus the page-aligned section offset.
/// `view_prot` uses the WRITECOPY translation so writes trigger kernel
/// CoW without requiring section write access.
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

/// `mmap(fd, MAP_SHARED, ...)` — write-through file view.
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

/// `mmap(... | MAP_HUGETLB, ...)` — large-page anonymous section.
/// Caller supplies the page-kind (decoded from `MAP_HUGE_*` bits) and
/// the section handle from `nt_pal::commit_replace_large`.
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

/// `brk` — single-shot anonymous reservation. The placeholder identity
/// covers a wide reservation (256 MiB by default) while the registered
/// range is the narrow `[base, cursor)` brk window. `placeholder_base`
/// and `placeholder_size` are populated explicitly so the substrate
/// retains the reservation across cursor extensions.
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta
brk_meta(int prot, void *placeholder_base, size_t placeholder_size) {
  ::LIBC_NAMESPACE::windows::va_tracker::AcquireMeta m;
  m.view_prot = posix_prot_to_page(prot);
  m.flags = ::LIBC_NAMESPACE::windows::va_tracker::region_flag::COMMITTED;
  m.placeholder_base = placeholder_base;
  m.placeholder_size = placeholder_size;
  return m;
}

/// `shm_open` followed by `mmap` — POSIX shm view. Caller supplies the
/// named section handle from `nt_pal::create_section_named`.
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

/// `shmat` — SysV shm view. Identical to `shm_posix_meta` in
/// substrate-side metadata; the segment registry lives at the POSIX
/// layer and is consulted separately.
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
