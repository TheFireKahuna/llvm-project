//===-- Raw page allocation for internal subsystems ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Foundation memory primitives for all libc subsystems. Sits below mmap,
// malloc, and every other allocator — used by subsystems that cannot depend
// on them without creating circular dependencies.
//
// Depends only on ntdll.h — no kernel32.h, no libc internals.
//
// Allocation model:
//   Reserve-commit   — page_reserve + page_commit. Linear growth.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PAGE_ALLOC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PAGE_ALLOC_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Reserve address space with no physical backing.
[[nodiscard]] LIBC_INLINE void *page_reserve(size_t size) {
  void *addr = nullptr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz, MEM_RESERVE, PAGE_NOACCESS, nullptr, 0);
  return NT_SUCCESS(st) ? addr : nullptr;
}

// Commit pages within a previously reserved range. Idempotent on
// already-committed pages.
[[nodiscard]] LIBC_INLINE bool page_commit(void *addr, size_t size) {
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz, MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
  return NT_SUCCESS(st);
}

// Commit pages within a reservation on a specific NUMA node. Node -1
// uses the system default. Idempotent on already-committed pages.
[[nodiscard]] LIBC_INLINE bool page_commit_numa(void *addr, size_t size, int numa_node) {
  SIZE_T sz = size;
  if (numa_node < 0)
    return page_commit(addr, size);
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = static_cast<ULONG>(numa_node);
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz, MEM_COMMIT, PAGE_READWRITE, &param, 1);
  return NT_SUCCESS(st);
}

// Return physical pages to the OS without releasing the VA reservation.
// Pages become PAGE_NOACCESS; next access requires page_commit.
LIBC_INLINE bool page_decommit(void *addr, size_t size) {
  SIZE_T sz = size;
  NTSTATUS st =
      ::NtFreeVirtualMemory(NtCurrentProcess(), &addr, &sz, MEM_DECOMMIT);
  return NT_SUCCESS(st);
}

// Change page protection on committed pages.
LIBC_INLINE bool page_protect(void *addr, size_t size, ULONG prot) {
  ULONG old;
  SIZE_T sz = size;
  NTSTATUS st =
      ::NtProtectVirtualMemory(NtCurrentProcess(), &addr, &sz, prot, &old);
  return NT_SUCCESS(st);
}

// Mark committed pages as discardable. Content may survive or be zero-filled
// on next access — no guarantee either way. Pages remain committed (no
// recommit needed). ~1.8x faster than decommit+recommit for recycling.
LIBC_INLINE void page_reset(void *addr, size_t size) {
  SIZE_T sz = size;
  ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &sz, MEM_RESET,
                               PAGE_READWRITE, nullptr, 0);
}

// Attempt to cancel a prior page_reset(). Returns true if all pages still
// contain their original content. Returns false if any page was reclaimed
// by the kernel (now zero-filled). Safe on non-reset pages (no-op).
[[nodiscard]] LIBC_INLINE bool page_reset_undo(void *addr, size_t size) {
  SIZE_T sz = size;
  return NT_SUCCESS(::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz, MEM_RESET_UNDO, PAGE_READWRITE,
      nullptr, 0));
}

// Reserve and commit pages. Returns nullptr on failure.
[[nodiscard]] LIBC_INLINE void *page_alloc(size_t size) {
  void *addr = nullptr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE, nullptr, 0);
  return NT_SUCCESS(st) ? addr : nullptr;
}

// Reserve and commit pages on a specific NUMA node. Node -1 uses default.
[[nodiscard]] LIBC_INLINE void *page_alloc_numa(size_t size, int numa_node) {
  if (numa_node < 0)
    return page_alloc(size);
  void *addr = nullptr;
  SIZE_T sz = size;
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = static_cast<ULONG>(numa_node);
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz, MEM_RESERVE | MEM_COMMIT,
      PAGE_READWRITE, &param, 1);
  return NT_SUCCESS(st) ? addr : nullptr;
}

// Release an entire allocation (VA + physical). Size must be 0 for
// MEM_RELEASE. Failure indicates a logic bug (double-free, wrong address).
LIBC_INLINE void page_free(void *addr) {
  SIZE_T sz = 0;
  NTSTATUS st =
      ::NtFreeVirtualMemory(NtCurrentProcess(), &addr, &sz, MEM_RELEASE);
  LIBC_ASSERT(NT_SUCCESS(st) && "page_free: NtFreeVirtualMemory failed");
  (void)st;
}

// Reserve address space at a specific base address. Returns the base on
// success, nullptr if the VA is already occupied or the address is invalid.
// The caller must pass a 64KB-aligned address (allocation granularity).
[[nodiscard]] LIBC_INLINE void *page_reserve_at(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz, MEM_RESERVE, PAGE_NOACCESS, nullptr, 0);
  return NT_SUCCESS(st) ? base : nullptr;
}

// ---------------------------------------------------------------------------
// Placeholder-based allocation for SlabPool
// ---------------------------------------------------------------------------

// Reserve placeholder VA (MEM_RESERVE_PLACEHOLDER). The placeholder can be
// split, committed, or preserved independently of other allocations.
[[nodiscard]] LIBC_INLINE void *placeholder_reserve(size_t size) {
  void *addr = nullptr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  return NT_SUCCESS(st) ? addr : nullptr;
}

// Reserve placeholder VA at a specific base address. Returns the base on
// success, nullptr if the VA is already occupied or the address is invalid.
// The caller must pass a 64KB-aligned address (allocation granularity).
// Used by brk_grow and any caller needing adjacent-VA placeholder growth.
[[nodiscard]] LIBC_INLINE void *placeholder_reserve_at(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  return NT_SUCCESS(st) ? base : nullptr;
}

// Commit pages into a placeholder region (MEM_REPLACE_PLACEHOLDER).
[[nodiscard]] LIBC_INLINE bool placeholder_commit(void *addr, size_t size) {
  SIZE_T sz = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &addr, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      nullptr, 0);
  return NT_SUCCESS(st);
}

// Convert committed pages back to a placeholder (decommit + preserve).
// Used by slab_pool to recycle slab bodies without releasing VA.
LIBC_INLINE void placeholder_preserve(void *addr, size_t size) {
  SIZE_T sz = size;
  NTSTATUS st = ::NtFreeVirtualMemory(NtCurrentProcess(), &addr, &sz,
                                      MEM_DECOMMIT | MEM_PRESERVE_PLACEHOLDER);
  LIBC_ASSERT(NT_SUCCESS(st) &&
              "placeholder_preserve: NtFreeVirtualMemory failed");
  (void)st;
}

// Split a placeholder at the given offset into two independent placeholders.
[[nodiscard]] LIBC_INLINE bool placeholder_split(void *addr, size_t split_offset) {
  PVOID base = addr;
  SIZE_T sz = split_offset;
  return NT_SUCCESS(::NtFreeVirtualMemory(
      NtCurrentProcess(), &base, &sz,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
}

// Coalesce adjacent placeholders covering [addr, addr+total_size) into a
// single placeholder. The span must be composed entirely of placeholders
// (no committed pages or section views); the kernel verifies and fails
// atomically otherwise. Used to undo earlier splits after commits are
// rolled back or ranges are preserved — e.g., brk_shrink coalescing the
// preserved tail with the pre-existing tail placeholder.
LIBC_INLINE bool placeholder_coalesce(void *addr, size_t total_size) {
  PVOID base = addr;
  SIZE_T sz = total_size;
  return NT_SUCCESS(::NtFreeVirtualMemory(
      NtCurrentProcess(), &base, &sz,
      MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PAGE_ALLOC_H
