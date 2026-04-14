//===-- RAII placeholder VA ownership guard ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PlaceholderRange owns a placeholder-reserved VA range. Move-only RAII.
// Destructor releases the VA via NtFreeVirtualMemory(MEM_RELEASE).
//
// Covers two placeholder lifecycle patterns:
//   1. Consumable: reserve → (optionally split) → consume/map/commit
//   2. Carve-up:   reserve → split → consume parts → release remainder
//
// Does NOT cover:
//   - CAS swap (preserve_to_placeholder → replace_placeholder_commit):
//     atomic state transitions on existing VA, no ownership to track.
//   - Coalesce-or-release: topology operation depending on neighbor state.
//   - Remap rollback: RemapGuard owns the envelope, raw addresses suffice.
//
// The destructor calls release_placeholder() (simple MEM_RELEASE), not
// coalesce_or_release_placeholder(). Coalesce is a caller decision —
// only munmap/vm_protect paths need it, and they call it explicitly.
//
// Thread safety: PlaceholderRange is not thread-safe. The underlying
// kernel operations are atomic (single-VAD-lock), but the wrapper's
// base_/size_ members are plain fields. One owner at a time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PLACEHOLDER_RANGE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PLACEHOLDER_RANGE_H

#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/view_spec.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class PlaceholderRange {
  void *base_ = nullptr;
  SIZE_T size_ = 0;

public:
  // -- Lifecycle --

  LIBC_INLINE PlaceholderRange() = default;

  LIBC_INLINE ~PlaceholderRange() { release(); }

  LIBC_INLINE PlaceholderRange(PlaceholderRange &&o) noexcept
      : base_(o.base_), size_(o.size_) {
    o.base_ = nullptr;
    o.size_ = 0;
  }

  LIBC_INLINE PlaceholderRange &operator=(PlaceholderRange &&o) noexcept {
    if (this != &o) {
      release();
      base_ = o.base_;
      size_ = o.size_;
      o.base_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }

  PlaceholderRange(const PlaceholderRange &) = delete;
  PlaceholderRange &operator=(const PlaceholderRange &) = delete;

  // -- Observers --

  LIBC_INLINE explicit operator bool() const { return base_ != nullptr; }
  LIBC_INLINE void *base() const { return base_; }
  LIBC_INLINE SIZE_T size() const { return size_; }

  // -- Creation --

  /// Reserve VA at a system-chosen or caller-specified address.
  /// Returns empty on failure.
  [[nodiscard]] LIBC_INLINE static PlaceholderRange
  reserve(SIZE_T size, void *addr = nullptr) {
    void *p = create_placeholder(addr, size);
    return p ? PlaceholderRange(p, size) : PlaceholderRange();
  }

  /// Reserve VA with the kernel-rounded actual size returned.
  /// The kernel may round up; actual_size receives the true extent.
  /// Returns empty on failure.
  [[nodiscard]] LIBC_INLINE static PlaceholderRange
  reserve_ex(SIZE_T requested, SIZE_T &actual_size, void *addr = nullptr) {
    void *p = create_placeholder_ex(addr, requested, actual_size);
    return p ? PlaceholderRange(p, actual_size) : PlaceholderRange();
  }

  /// Reserve VA constrained to the low 2 GB (MAP_32BIT emulation).
  /// Returns empty on failure.
  [[nodiscard]] LIBC_INLINE static PlaceholderRange
  reserve_32bit(SIZE_T size) {
    void *p = create_placeholder_32bit(size);
    return p ? PlaceholderRange(p, size) : PlaceholderRange();
  }

  /// Reserve VA with NUMA node affinity.
  /// Returns empty on failure.
  [[nodiscard]] LIBC_INLINE static PlaceholderRange
  reserve_numa(SIZE_T size, ULONG numa_node, void *addr = nullptr) {
    void *p = create_placeholder_numa(addr, size, numa_node);
    return p ? PlaceholderRange(p, size) : PlaceholderRange();
  }

  /// Reserve with retry and yield between attempts. Centralizes the
  /// retry-yield-retry pattern used across MAP_FIXED and mremap paths.
  /// Returns empty if all attempts fail.
  [[nodiscard]] LIBC_INLINE static PlaceholderRange
  reserve_with_retry(SIZE_T size, void *addr = nullptr, int max_attempts = 3) {
    for (int i = 0; i < max_attempts; ++i) {
      PlaceholderRange ph = reserve(size, addr);
      if (ph)
        return ph;
      ::NtYieldExecution();
    }
    return PlaceholderRange();
  }

  /// Reserve at an exact address with retry. Returns empty if the kernel
  /// cannot place the reservation at the requested address after all attempts.
  /// Used by MREMAP_FIXED and MAP_FIXED paths that need a specific VA.
  [[nodiscard]] LIBC_INLINE static PlaceholderRange
  reserve_exact_with_retry(SIZE_T size, void *addr, int max_attempts = 3) {
    for (int i = 0; i < max_attempts; ++i) {
      PlaceholderRange ph = reserve(size, addr);
      if (ph && ph.base() == addr)
        return ph;
      ph.release();
      ::NtYieldExecution();
    }
    return PlaceholderRange();
  }

  // -- Adoption --

  /// Wrap a raw (base, size) known to be a valid placeholder.
  /// Used when adopting from unmap-preserve or from pre-PlaceholderRange code.
  /// Caller asserts the VA is a live placeholder — no validation performed.
  static LIBC_INLINE PlaceholderRange from_raw(void *base, SIZE_T size) {
    return PlaceholderRange(base, size);
  }

  // -- Consumption --

  /// Yield the raw (base, size) and null this object. After consume(),
  /// the caller owns the VA directly — typically because it is about to be
  /// replaced by a section view or committed pages.
  struct Consumed {
    void *base;
    SIZE_T size;
  };

  [[nodiscard]] LIBC_INLINE Consumed consume() {
    LIBC_ASSERT(base_ && "consume() on empty PlaceholderRange");
    Consumed c{base_, size_};
    base_ = nullptr;
    size_ = 0;
    return c;
  }

  /// Map a section view into this placeholder via ViewSpec.
  /// On success: nulls this object (placeholder consumed by the view).
  /// On failure: this object remains valid (placeholder still reserved).
  /// Returns raw NTSTATUS for caller-specific error handling.
  LIBC_INLINE NTSTATUS map(const ViewSpec &spec) {
    LIBC_ASSERT(base_ && "map() on empty PlaceholderRange");
    NTSTATUS st = spec.map_into(base_, size_);
    if (NT_SUCCESS(st)) {
      base_ = nullptr;
      size_ = 0;
    }
    return st;
  }

  /// Map a section view with extended parameters (NUMA affinity, etc.).
  /// Same ownership semantics as map().
  LIBC_INLINE NTSTATUS map_ex(const ViewSpec &spec,
                               MEM_EXTENDED_PARAMETER *params,
                               ULONG param_count) {
    LIBC_ASSERT(base_ && "map_ex() on empty PlaceholderRange");
    NTSTATUS st = spec.map_into_ex(base_, size_, params, param_count);
    if (NT_SUCCESS(st)) {
      base_ = nullptr;
      size_ = 0;
    }
    return st;
  }

  /// Map as SEC_RESERVE (demand-commit via VEH).
  /// Same ownership semantics as map().
  LIBC_INLINE NTSTATUS map_reserve(const ViewSpec &spec) {
    LIBC_ASSERT(base_ && "map_reserve() on empty PlaceholderRange");
    NTSTATUS st = spec.map_into_reserve(base_, size_);
    if (NT_SUCCESS(st)) {
      base_ = nullptr;
      size_ = 0;
    }
    return st;
  }

  /// Commit private pages into this placeholder (MAP_ANONYMOUS|MAP_PRIVATE).
  /// On success: nulls this object (placeholder consumed by committed pages).
  /// On failure: this object remains valid (placeholder still reserved).
  /// Returns raw NTSTATUS.
  LIBC_INLINE NTSTATUS commit(DWORD prot) {
    LIBC_ASSERT(base_ && "commit() on empty PlaceholderRange");
    NTSTATUS st = replace_placeholder_commit(base_, size_, prot);
    if (NT_SUCCESS(st)) {
      base_ = nullptr;
      size_ = 0;
    }
    return st;
  }

  /// Commit and abandon: detaches unconditionally, then attempts commit.
  /// On success: VA holds committed private pages.
  /// On failure: VA survives as a bare placeholder (no destructor release).
  ///
  /// Unlike commit(), failure does NOT keep the PlaceholderRange valid —
  /// the object is empty regardless of outcome. This is the correct
  /// primitive when the caller wants the placeholder to persist as a
  /// PROT_NONE reservation on failure rather than being freed to MEM_FREE.
  LIBC_INLINE NTSTATUS commit_or_abandon(DWORD prot) {
    LIBC_ASSERT(base_ && "commit_or_abandon() on empty PlaceholderRange");
    void *b = base_;
    SIZE_T s = size_;
    base_ = nullptr;
    size_ = 0;
    return replace_placeholder_commit(b, s, prot);
  }

  /// Reserve-replace: placeholder → MEM_RESERVE private (no commit).
  /// Used for MAP_NORESERVE demand-commit via VEH. Pages are committed
  /// on demand by the fault handler reading AllocationProtect from MBI.
  /// Same ownership semantics as commit(): consumes on success, valid on failure.
  LIBC_INLINE NTSTATUS reserve_replace(DWORD prot) {
    LIBC_ASSERT(base_ && "reserve_replace() on empty PlaceholderRange");
    NTSTATUS st = replace_placeholder_reserve(base_, size_, prot);
    if (NT_SUCCESS(st)) {
      base_ = nullptr;
      size_ = 0;
    }
    return st;
  }

  // -- Split --

  /// Split at byte offset into two independent placeholders.
  /// On success: consumes self, populates *left and *right.
  /// On failure: self remains valid (unsplit), outputs untouched.
  ///
  /// The kernel's NtFreeVirtualMemory(MEM_PRESERVE_PLACEHOLDER) atomically
  /// creates two placeholders from one. Both halves are captured — the
  /// compiler prevents forgetting either.
  LIBC_INLINE bool split(SIZE_T offset, PlaceholderRange *left,
                         PlaceholderRange *right) {
    LIBC_ASSERT(base_ && "split() on empty PlaceholderRange");
    if (!split_placeholder(base_, offset))
      return false;
    char *b = static_cast<char *>(base_);
    *left = PlaceholderRange(b, offset);
    *right = PlaceholderRange(b + offset, size_ - offset);
    base_ = nullptr;
    size_ = 0;
    return true;
  }

  // -- Release --

  /// Release the placeholder VA entirely (return to MEM_FREE).
  /// Safe to call on empty objects (no-op).
  ///
  /// This is a simple MEM_RELEASE, NOT coalesce_or_release_placeholder().
  /// Coalesce is a topology decision the caller makes explicitly when
  /// releasing placeholders adjacent to live mappings.
  LIBC_INLINE void release() {
    if (base_) {
      release_placeholder(base_);
      base_ = nullptr;
      size_ = 0;
    }
  }

private:
  /// Private constructor — all construction goes through factory methods
  /// (reserve, from_raw) to make the provenance of each placeholder explicit.
  LIBC_INLINE PlaceholderRange(void *base, SIZE_T size)
      : base_(base), size_(size) {}
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PLACEHOLDER_RANGE_H
