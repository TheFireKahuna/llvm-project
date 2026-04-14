//===-- Windows page size utilities -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cached system page size and allocation granularity for Windows.
//
// Windows has two alignment levels:
//   - Page size (4KB on x64): protection, commit, and placeholder splits.
//   - Allocation granularity (64KB): NtAllocateVirtualMemoryEx base addresses
//     and NtMapViewOfSectionEx section offsets.
//
// Placeholder splits (NtFreeVirtualMemory with MEM_PRESERVE_PLACEHOLDER) are
// page-granular, not 64KB-granular. This enables page-exact munmap, mremap,
// and NUMA interleave — matching Linux's page-granular mmap semantics.
//
// All values are populated once by pcb_startup_init() (Phase 0 of
// __libc_dll_init()) into the Process Control Block (g_pcb). The accessors
// here are branchless reads from the PCB — no lazy init, no atomic fence
// on every call.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PAGE_SIZE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PAGE_SIZE_H

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h> // SIZE_MAX

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Get the system page size (typically 4096 on x64).
// Populated by pcb_startup_init() before any subsystem init.
LIBC_INLINE SIZE_T get_page_size() {
  return static_cast<SIZE_T>(g_pcb.zone0.page_size());
}

// Source-compatible aliases.
LIBC_INLINE SIZE_T get_cached_page_size() { return get_page_size(); }

LIBC_INLINE SIZE_T get_cached_page_mask() { return get_page_size() - 1; }

// Get the allocation granularity (typically 65536 on x64).
// VirtualAlloc base addresses must be aligned to this.
LIBC_INLINE SIZE_T get_alloc_granularity() {
  return static_cast<SIZE_T>(g_pcb.zone0.alloc_granularity());
}

// Get the large page minimum (typically 2MB on x64). Returns 0 if large pages
// are not supported. Wraps the ntdll.h inline that reads KUSER_SHARED_DATA.
LIBC_INLINE SIZE_T get_large_page_minimum() {
  return ::GetLargePageMinimum();
}

// Get the lowest valid application address.
LIBC_INLINE void *get_min_address() { return g_pcb.zone0.min_address(); }

// Get the highest valid application address.
LIBC_INLINE void *get_max_address() { return g_pcb.zone0.max_address(); }

/// Round \p size up to the nearest multiple of \p align.
/// \p align must be a power of two. Returns 0 on overflow.
LIBC_INLINE SIZE_T round_up_to_align(SIZE_T size, SIZE_T align) {
  const SIZE_T mask = align - 1;
  if (LIBC_UNLIKELY(size > SIZE_MAX - mask))
    return 0;
  return (size + mask) & ~mask;
}

// Round size up to page boundary. Returns 0 on overflow.
LIBC_INLINE SIZE_T round_to_page(SIZE_T size) {
  return round_up_to_align(size, get_page_size());
}

// Check if address is page-aligned.
LIBC_INLINE bool is_page_aligned(const void *addr) {
  return (reinterpret_cast<uintptr_t>(addr) & (get_page_size() - 1)) == 0;
}

// Check if address is aligned to allocation granularity.
LIBC_INLINE bool is_alloc_aligned(const void *addr) {
  return (reinterpret_cast<uintptr_t>(addr) & (get_alloc_granularity() - 1)) ==
         0;
}

// Round a section offset down to page boundary.
// NtMapViewOfSectionEx accepts page-aligned offsets (not just 64KB).
LIBC_INLINE DWORD64 round_down_to_page_offset(DWORD64 offset) {
  return offset & ~(static_cast<DWORD64>(get_page_size()) - 1);
}

// Round an address down to allocation granularity.
LIBC_INLINE uintptr_t align_down_to_granularity(uintptr_t addr) {
  return addr & ~(static_cast<uintptr_t>(get_alloc_granularity()) - 1);
}

// Round an address up to allocation granularity. Returns 0 on overflow.
LIBC_INLINE uintptr_t align_up_to_granularity(uintptr_t addr) {
  const uintptr_t mask = get_alloc_granularity() - 1;
  if (LIBC_UNLIKELY(addr > UINTPTR_MAX - mask))
    return 0;
  return (addr + mask) & ~mask;
}

// Round an address down to page boundary.
// Placeholder splits are page-granular (not 64KB-granular).
LIBC_INLINE uintptr_t align_down_to_page(uintptr_t addr) {
  return addr & ~(static_cast<uintptr_t>(get_page_size()) - 1);
}

// Round an address up to page boundary. Returns 0 on overflow.
LIBC_INLINE uintptr_t align_up_to_page(uintptr_t addr) {
  const uintptr_t mask = get_page_size() - 1;
  if (LIBC_UNLIKELY(addr > UINTPTR_MAX - mask))
    return 0;
  return (addr + mask) & ~mask;
}

/// Convert POSIX protection flags to Windows page protection constants.
/// PROT_* values: PROT_NONE=0, PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4.
/// Windows has no write-only pages; PROT_WRITE promotes to PAGE_READWRITE.
/// Unknown high bits are silently ignored (only PROT_READ/WRITE/EXEC checked),
/// matching Linux which masks prot to 3 bits before processing.
LIBC_INLINE DWORD prot_to_page_flags(int prot) {
  if (prot == PROT_NONE)
    return PAGE_NOACCESS;

  const bool r = (prot & PROT_READ) != 0;
  const bool w = (prot & PROT_WRITE) != 0;
  const bool x = (prot & PROT_EXEC) != 0;

  if (x && w)
    return PAGE_EXECUTE_READWRITE;
  if (x && r)
    return PAGE_EXECUTE_READ;
  if (x)
    return PAGE_EXECUTE_READ; // PAGE_EXECUTE alone rejected by CFG/CET policies
  if (w)
    return PAGE_READWRITE;
  if (r)
    return PAGE_READONLY;

  return PAGE_NOACCESS;
}

/// Convert POSIX protection to Windows page flags for MAP_PRIVATE file views.
/// MAP_PRIVATE + PROT_WRITE uses copy-on-write (PAGE_WRITECOPY) so writes
/// don't propagate to the underlying file.
LIBC_INLINE DWORD prot_to_page_flags_cow(int prot) {
  if (prot == PROT_NONE)
    return PAGE_NOACCESS;

  const bool r = (prot & PROT_READ) != 0;
  const bool w = (prot & PROT_WRITE) != 0;
  const bool x = (prot & PROT_EXEC) != 0;

  if (x && w)
    return PAGE_EXECUTE_WRITECOPY;
  if (x && r)
    return PAGE_EXECUTE_READ;
  if (x)
    return PAGE_EXECUTE_READ;
  if (w)
    return PAGE_WRITECOPY;
  if (r)
    return PAGE_READONLY;

  return PAGE_NOACCESS;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PAGE_SIZE_H
