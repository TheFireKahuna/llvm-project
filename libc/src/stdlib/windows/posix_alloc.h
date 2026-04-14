//===-- POSIX mmap-based slab allocator --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-tier allocator built entirely on POSIX mmap/munmap/mprotect/madvise.
//
// Tier 1 — Thread Cache (lock-free):
//   Per-thread free lists for 23 size classes, accessed via FLS/TEB TLS.
//   malloc/free hit this path with zero locking.
//   Freelist pointers XOR-encoded (per-segment cookie + slot address).
//
// Tier 2 — Central Slab Arena (per-class lock):
//   2MB segments (2MB-aligned) divided into 30 x 64KB slot pages.
//   Page index 0 is a guard (PROT_NONE) isolating metadata from slots.
//   Each page serves one size class with bump + free list allocation.
//   Cross-thread returns via atomic CAS free list (cache-line-split from
//   owner-only state to eliminate false sharing).
//   Freed pages recycled via ring buffer + madvise(MADV_DONTNEED).
//
// Tier 3 — Large (direct mmap):
//   Allocations > 32KB go through mmap/munmap directly.
//   LargeHeader prepended at the mmap base.
//
// Hardening:
//   - Freelist pointers XOR'd with per-segment cookie + storage address
//   - Double-free canary (cookie-derived) at slot+8 in every freed slot
//   - Zero-on-free for all slab size classes (prevents info leaks)
//   - Guard page between segment metadata and slot pages (MMU-enforced)
//   - Cross-thread xthread array on separate cache line from owner state
//
// Pointer-to-segment: ptr & ~(2MB-1)  (single AND)
// Pointer-to-page:    ((ptr & (2MB-1)) >> 16) - 1
// No per-object headers for slab allocations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDLIB_WINDOWS_POSIX_ALLOC_H
#define LLVM_LIBC_SRC_STDLIB_WINDOWS_POSIX_ALLOC_H

#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {

/// Minimum alignment guaranteed by malloc (x64/AArch64).
inline constexpr size_t MALLOC_ALIGN = 16;

/// Allocations above this threshold go directly through mmap.
inline constexpr size_t LARGE_THRESHOLD = 32768;

/// Eager initialization — call from startup before any allocation.
void posix_alloc_init();

void *posix_alloc(size_t size);
void *posix_alloc_zeroed(size_t size);
void *posix_alloc_aligned(size_t size, size_t alignment);
void posix_free(void *ptr);
void *posix_realloc(void *ptr, size_t size);
size_t posix_usable_size(void *ptr);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDLIB_WINDOWS_POSIX_ALLOC_H
