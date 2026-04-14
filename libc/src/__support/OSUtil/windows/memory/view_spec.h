//===-- Section view descriptor (ViewSpec) -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ViewSpec is a pure value type that describes "what to map" into a
// placeholder VA range. It bundles the five correlated parameters that
// every NtMapViewOfSectionEx call requires:
//
//   (section_handle, file_handle, offset, prot, flags)
//
// Key operations:
//   at_offset(delta)      — derive a sub-view for split-remap fragments
//   map_into(base, size)  — NtMapViewOfSectionEx with MEM_REPLACE_PLACEHOLDER
//   map_into_ex(...)      — same, with extended parameters (NUMA, etc.)
//   map_into_reserve(...) — same, with MEM_RESERVE for SEC_RESERVE views
//   from_entry(e)         — construct from a MappingEntry
//
// ViewSpec does not own any handles. The section handle is borrowed from
// the mapping table, a SectionHandle, an OpenFileDescription, or a
// RemapGuard's duplicated handle. Making ViewSpec owning would create
// double-close hazards and make at_offset() impossible without handle
// duplication.
//
// map_into() takes raw (void *, SIZE_T) rather than PlaceholderRange
// because remap paths often work with raw addresses from the mapping
// table or split operations. The caller-facing PlaceholderRange::map()
// provides the typed entry point for callers that have a PlaceholderRange.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VIEW_SPEC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VIEW_SPEC_H

#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Forward declaration — full definition in mapping_table.h.
struct MappingEntry;

/// Pure value type describing "what to map" into a VA placeholder.
///
/// All handles are borrowed — ViewSpec never closes anything.
/// Copyable, movable, no RAII, no heap.
struct ViewSpec {
  HANDLE section;       // Section object (borrowed)
  HANDLE file;          // Backing file for msync (borrowed, may be null)
  LARGE_INTEGER offset; // Byte offset into section
  DWORD prot;           // Page protection (PAGE_READWRITE, etc.)
  DWORD flags;          // VM_FLAG_* metadata (SEC_RESERVE, FILE_PRIVATE, etc.)

  /// Derive a sub-view at a byte delta from this view's offset.
  /// This is the primitive for split-remap fragment derivation:
  ///
  ///   ViewSpec right = entry.spec.at_offset(target_end - view_base);
  ///
  /// Replaces the manual LARGE_INTEGER arithmetic scattered across
  /// remap_transaction.cpp, region_snapshot.h, mremap_engine.cpp, etc.
  LIBC_INLINE constexpr ViewSpec at_offset(SIZE_T delta) const {
    ViewSpec s = *this;
    s.offset.QuadPart += static_cast<LONGLONG>(delta);
    return s;
  }

  /// Derive a view with a different page protection.
  /// Used when mprotect changes protection on a view that later needs
  /// remapping (split-remap after mprotect, NUMA rebind with new prot).
  LIBC_INLINE constexpr ViewSpec with_prot(DWORD new_prot) const {
    ViewSpec s = *this;
    s.prot = new_prot;
    return s;
  }

  /// Equality — all five fields match. Used for round-trip testing
  /// (from_entry → MappingEntry → from_entry) and debug assertions.
  LIBC_INLINE friend constexpr bool operator==(const ViewSpec &a,
                                                const ViewSpec &b) {
    return a.section == b.section && a.file == b.file &&
           a.offset.QuadPart == b.offset.QuadPart && a.prot == b.prot &&
           a.flags == b.flags;
  }

  LIBC_INLINE friend constexpr bool operator!=(const ViewSpec &a,
                                                const ViewSpec &b) {
    return !(a == b);
  }

  /// Map this spec into a placeholder, consuming the placeholder VA.
  /// Calls NtMapViewOfSectionEx with MEM_REPLACE_PLACEHOLDER.
  /// Returns raw NTSTATUS for caller-specific error handling.
  LIBC_INLINE NTSTATUS map_into(void *base, SIZE_T size) const {
    PVOID b = base;
    SIZE_T sz = size;
    LARGE_INTEGER off = offset;
    return ::NtMapViewOfSectionEx(section, NtCurrentProcess(), &b, &off, &sz,
                                  MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
  }

  /// Map with extended parameters (NUMA affinity, etc.).
  LIBC_INLINE NTSTATUS map_into_ex(void *base, SIZE_T size,
                                    MEM_EXTENDED_PARAMETER *params,
                                    ULONG param_count) const {
    PVOID b = base;
    SIZE_T sz = size;
    LARGE_INTEGER off = offset;
    return ::NtMapViewOfSectionEx(section, NtCurrentProcess(), &b, &off, &sz,
                                  MEM_REPLACE_PLACEHOLDER, prot, params,
                                  param_count);
  }

  /// Map as SEC_RESERVE (demand-commit via VEH). The view is mapped
  /// reserved; the VEH handler commits pages on first access.
  LIBC_INLINE NTSTATUS map_into_reserve(void *base, SIZE_T size) const {
    PVOID b = base;
    SIZE_T sz = size;
    LARGE_INTEGER off = offset;
    return ::NtMapViewOfSectionEx(section, NtCurrentProcess(), &b, &off, &sz,
                                  MEM_REPLACE_PLACEHOLDER | MEM_RESERVE, prot,
                                  nullptr, 0);
  }

  /// Construct from a MappingEntry. Extracts the five backing parameters.
  ///
  /// Requires MappingEntry to be a complete type. Callers that include
  /// both view_spec.h and mapping_table.h get this for free. The function
  /// is defined after the MappingEntry definition below.
  static LIBC_INLINE ViewSpec from_entry(const MappingEntry &e);
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VIEW_SPEC_H
