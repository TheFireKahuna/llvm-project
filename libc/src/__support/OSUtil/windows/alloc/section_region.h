//===-- Owning section + view convenience wrapper ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SectionRegion composes SectionHandle + mapped view into a single owning
// type for the common "create section, map it, use it, destroy it" pattern.
//
// Replaces the old SectionRegion in alloc/page_alloc.h with a primitive
// built on PlaceholderRange + SectionHandle, providing:
//   - Placeholder-backed mapping (no VA race windows)
//   - Named section support (signal mailbox, FIFO)
//   - Handle release without unmapping (spawn handle inheritance)
//   - Alias views of the same section (pipe/socketpair)
//
// This is the highest-level primitive — built on SectionHandle,
// PlaceholderRange, and SectionView. Most internal infrastructure
// (fd table, mapping table guards, pkey state) should use this type
// instead of raw NtCreateSectionEx/NtMapViewOfSectionEx sequences.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SECTION_REGION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SECTION_REGION_H

#include "src/__support/CPP/utility/move.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/alloc/section_view.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class SectionRegion {
  SectionHandle section_;
  void *base_ = nullptr;
  SIZE_T size_ = 0;

public:
  // -- Lifecycle --

  LIBC_INLINE SectionRegion() = default;

  LIBC_INLINE ~SectionRegion() { destroy(); }

  LIBC_INLINE SectionRegion(SectionRegion &&o) noexcept
      : section_(cpp::move(o.section_)), base_(o.base_), size_(o.size_) {
    o.base_ = nullptr;
    o.size_ = 0;
  }

  LIBC_INLINE SectionRegion &operator=(SectionRegion &&o) noexcept {
    if (this != &o) {
      destroy();
      section_ = cpp::move(o.section_);
      base_ = o.base_;
      size_ = o.size_;
      o.base_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }

  SectionRegion(const SectionRegion &) = delete;
  SectionRegion &operator=(const SectionRegion &) = delete;

  // -- Observers --

  LIBC_INLINE explicit operator bool() const { return base_ != nullptr; }
  LIBC_INLINE void *base() const { return base_; }
  LIBC_INLINE SIZE_T size() const { return size_; }
  LIBC_INLINE HANDLE handle() const { return section_.get(); }

  template <typename T> LIBC_INLINE T *as() const {
    return static_cast<T *>(base_);
  }

  // -- Creation: Anonymous --

  /// Anonymous pagefile-backed create-and-map. 3 syscalls:
  ///   1. NtAllocateVirtualMemoryEx (reserve placeholder)
  ///   2. NtCreateSectionEx (create pagefile section)
  ///   3. NtMapViewOfSectionEx (replace placeholder with view)
  ///
  /// Returns empty SectionRegion on failure (no partial state).
  /// This is the direct replacement for the old SectionRegion::create()
  /// in page_alloc.h.
  [[nodiscard]] LIBC_INLINE static SectionRegion
  create_anon(SIZE_T size, DWORD prot = PAGE_READWRITE,
              NTSTATUS *st = nullptr) {
    // Reserve placeholder.
    PlaceholderRange ph = PlaceholderRange::reserve(size);
    if (!ph) {
      if (st)
        *st = STATUS_NO_MEMORY;
      return SectionRegion();
    }

    // Create pagefile-backed section.
    NTSTATUS sec_st;
    SectionHandle section = SectionHandle::create_anon_rw(ph.size(), &sec_st);
    if (!section) {
      if (st)
        *st = sec_st;
      return SectionRegion(); // ~ph releases placeholder
    }

    // Map section into placeholder via SectionView.
    SectionView view =
        SectionView::map_placeholder(ph, section, prot, {}, 0, st);
    if (!view)
      return SectionRegion(); // ~section closes handle, ~ph releases VA

    SectionRegion r;
    r.section_ = cpp::move(section);
    r.size_ = view.size();
    r.base_ = view.detach();
    if (st)
      *st = STATUS_SUCCESS;
    return r;
  }

  // -- Creation: From Existing Handle --

  /// Map an existing section handle. Ownership transfers on success only:
  /// if mapping succeeds, SectionRegion adopts the handle (will close it).
  /// If mapping fails, the caller still owns the handle and must close it.
  ///
  /// Placeholder-backed: 2 syscalls (reserve + map).
  [[nodiscard]] LIBC_INLINE static SectionRegion
  map_existing(HANDLE section, SIZE_T size, DWORD prot = PAGE_READWRITE,
               NTSTATUS *st = nullptr) {
    PlaceholderRange ph = PlaceholderRange::reserve(size);
    if (!ph) {
      if (st)
        *st = STATUS_NO_MEMORY;
      return SectionRegion();
    }

    // Borrow handle for the map call — adopt only on success.
    SectionHandle borrowed = SectionHandle::borrow(section);
    SectionView view =
        SectionView::map_placeholder(ph, borrowed, prot, {}, 0, st);
    if (!view)
      return SectionRegion(); // ~ph releases VA. Caller still owns handle.

    SectionRegion r;
    r.section_ = SectionHandle::adopt(section);
    r.size_ = view.size();
    r.base_ = view.detach();
    return r;
  }

  // -- Creation: Named --

  /// Create or open a named section and map it. Used by signal_mailbox.
  [[nodiscard]] LIBC_INLINE static SectionRegion
  create_named(nt_wstring_view *name, SIZE_T size,
               DWORD prot = PAGE_READWRITE,
               PSECURITY_DESCRIPTOR sd = nullptr,
               NTSTATUS *st = nullptr) {
    NTSTATUS sec_st;
    SectionHandle section = SectionHandle::create_named(
        name, size,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
        PAGE_READWRITE, SEC_COMMIT, sd, &sec_st);
    if (!section) {
      if (st)
        *st = sec_st;
      return SectionRegion();
    }

    // Named sections: map at kernel-chosen address (no placeholder needed).
    SectionView view =
        SectionView::map_anywhere(section, prot, 0, {}, st);
    if (!view)
      return SectionRegion(); // ~section closes handle

    SectionRegion r;
    r.section_ = cpp::move(section);
    r.size_ = view.size();
    r.base_ = view.detach();
    return r;
  }

  /// Open an existing named section and map it.
  [[nodiscard]] LIBC_INLINE static SectionRegion
  open_named(nt_wstring_view *name,
             ACCESS_MASK access = SECTION_MAP_READ | SECTION_MAP_WRITE,
             DWORD prot = PAGE_READWRITE, SIZE_T map_size = 0,
             NTSTATUS *st = nullptr) {
    NTSTATUS sec_st;
    SectionHandle section =
        SectionHandle::open_named(name, access, &sec_st);
    if (!section) {
      if (st)
        *st = sec_st;
      return SectionRegion();
    }

    SectionView view =
        SectionView::map_anywhere(section, prot, map_size, {}, st);
    if (!view)
      return SectionRegion(); // ~section closes handle

    SectionRegion r;
    r.section_ = cpp::move(section);
    r.size_ = view.size();
    r.base_ = view.detach();
    return r;
  }

  // -- Alias View --

  /// Map an additional independent view of the same section at a
  /// kernel-chosen address. Used by pipe/socketpair for the second
  /// channel's view. The returned SectionView is independently owned.
  [[nodiscard]] LIBC_INLINE SectionView
  map_alias(DWORD prot = PAGE_READWRITE, NTSTATUS *st = nullptr) const {
    return SectionView::map_anywhere(section_, prot, 0, {}, st);
  }

  // -- Destruction --

  /// Unmap view and close section handle.
  /// Safe to call on empty regions (no-op).
  LIBC_INLINE void destroy() {
    if (base_) {
      ::NtUnmapViewOfSectionEx(NtCurrentProcess(), base_, 0);
      base_ = nullptr;
      size_ = 0;
    }
    // ~section_ handles NtClose if owned.
    section_ = SectionHandle();
  }

  // -- Transfer --

  /// Release the section handle without unmapping the view.
  /// Used by spawn_ops for handle inheritance: the child inherits the
  /// handle, the view stays mapped in the parent.
  LIBC_INLINE HANDLE release_handle() { return section_.release(); }

  // -- ViewSpec --

  /// Build a ViewSpec from this region's section (for re-mapping).
  LIBC_INLINE ViewSpec view_spec(DWORD prot) const {
    return section_.view_spec(prot);
  }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SECTION_REGION_H
