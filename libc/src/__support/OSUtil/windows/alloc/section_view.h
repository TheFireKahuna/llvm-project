//===-- RAII mapped section view -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SectionView represents a section mapped into the process VA space.
// Move-only RAII — destructor unmaps the view.
//
// Intended for non-memory subsystems (signal mailbox, FIFO channels,
// spawn state blocks) where the full mapping table / remap protocol is
// NOT involved. The memory subsystem's mmap/mremap paths use
// PlaceholderRange + ViewSpec directly because the mapping table — not
// SectionView — owns view metadata for those paths.
//
// Two mapping modes:
//   - map_placeholder(): replaces a PlaceholderRange (consumes it on success)
//   - map_anywhere():    kernel-chosen address (no placeholder needed)
//
// unmap_release() (default destructor): unmaps and frees the VA.
// unmap_preserve(): unmaps but preserves VA as a placeholder (returns
//   PlaceholderRange for the caller to reuse).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SECTION_VIEW_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SECTION_VIEW_H

#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class SectionView {
  void *base_ = nullptr;
  SIZE_T size_ = 0;

public:
  // -- Lifecycle --

  LIBC_INLINE SectionView() = default;

  LIBC_INLINE ~SectionView() { unmap_release(); }

  LIBC_INLINE SectionView(SectionView &&o) noexcept
      : base_(o.base_), size_(o.size_) {
    o.base_ = nullptr;
    o.size_ = 0;
  }

  LIBC_INLINE SectionView &operator=(SectionView &&o) noexcept {
    if (this != &o) {
      unmap_release();
      base_ = o.base_;
      size_ = o.size_;
      o.base_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }

  SectionView(const SectionView &) = delete;
  SectionView &operator=(const SectionView &) = delete;

  // -- Observers --

  LIBC_INLINE explicit operator bool() const { return base_ != nullptr; }
  LIBC_INLINE void *base() const { return base_; }
  LIBC_INLINE SIZE_T size() const { return size_; }

  template <typename T> LIBC_INLINE T *as() const {
    return static_cast<T *>(base_);
  }

  // -- Mapping: Into Placeholder --

  /// Map a section into a PlaceholderRange (consuming the placeholder).
  /// On success: placeholder is consumed, SectionView owns the mapped VA.
  /// On failure: placeholder remains valid, returns empty SectionView.
  ///
  /// If map_size is 0, maps the full placeholder extent.
  [[nodiscard]] LIBC_INLINE static SectionView
  map_placeholder(PlaceholderRange &ph, const SectionHandle &section,
                  DWORD prot, LARGE_INTEGER offset = {},
                  SIZE_T map_size = 0, NTSTATUS *st = nullptr) {
    SIZE_T sz = map_size ? map_size : ph.size();
    PVOID base = ph.base();
    LARGE_INTEGER off = offset;
    NTSTATUS status = ::NtMapViewOfSectionEx(
        section.get(), NtCurrentProcess(), &base, &off, &sz,
        MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
    if (st)
      *st = status;
    if (!NT_SUCCESS(status))
      return SectionView();
    // Placeholder consumed — null it without releasing.
    (void)ph.consume();
    return SectionView(base, sz);
  }

  // -- Mapping: Kernel-Chosen Address --

  /// Map a section at a kernel-chosen address. No placeholder needed.
  /// Used by non-mmap subsystems (signal mailbox, FIFO channels) that
  /// don't need placeholder discipline.
  [[nodiscard]] LIBC_INLINE static SectionView
  map_anywhere(const SectionHandle &section, DWORD prot,
               SIZE_T map_size = 0, LARGE_INTEGER offset = {},
               NTSTATUS *st = nullptr) {
    PVOID base = nullptr;
    SIZE_T sz = map_size;
    LARGE_INTEGER off = offset;
    NTSTATUS status = ::NtMapViewOfSectionEx(
        section.get(), NtCurrentProcess(), &base, &off, &sz, 0, prot,
        nullptr, 0);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionView(base, sz) : SectionView();
  }

  // -- Unmap --

  /// Unmap the view and preserve the VA as a placeholder.
  /// Returns a PlaceholderRange wrapping the preserved VA.
  /// On failure: returns empty PlaceholderRange. The view remains mapped
  /// and will be released (not preserved) by the destructor's unmap_release().
  ///
  /// Used when the VA needs to be reused for a different mapping.
  [[nodiscard]] LIBC_INLINE PlaceholderRange unmap_preserve() {
    if (!base_)
      return PlaceholderRange();
    NTSTATUS status = ::NtUnmapViewOfSectionEx(
        NtCurrentProcess(), base_, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
    if (!NT_SUCCESS(status))
      return PlaceholderRange();
    PlaceholderRange ph = PlaceholderRange::from_raw(base_, size_);
    base_ = nullptr;
    size_ = 0;
    return ph;
  }

  /// Unmap the view and release the VA entirely (return to MEM_FREE).
  /// Safe to call on empty views (no-op). This is the destructor action.
  LIBC_INLINE void unmap_release() {
    if (base_) {
      NTSTATUS st = ::NtUnmapViewOfSectionEx(NtCurrentProcess(), base_, 0);
      LIBC_ASSERT(NT_SUCCESS(st) && "unmap_release: NtUnmapViewOfSectionEx failed");
      (void)st;
      base_ = nullptr;
      size_ = 0;
    }
  }

  /// Detach from the view without unmapping. Caller takes raw ownership.
  /// Returns the base address. Used when transferring to external ownership
  /// (e.g., the mapping table or a global pointer).
  [[nodiscard]] LIBC_INLINE void *detach() {
    void *b = base_;
    base_ = nullptr;
    size_ = 0;
    return b;
  }

private:
  LIBC_INLINE SectionView(void *base, SIZE_T size)
      : base_(base), size_(size) {}
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SECTION_VIEW_H
