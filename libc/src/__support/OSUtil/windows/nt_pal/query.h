//===-- nt_pal::query — VA region query + bulk walk --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Layer 0 PAL — VA query primitives. The kernel's MBI / MRI / bulk
// VAD-walk surface, factored into a few primitives plus a zero-overhead
// `RegionWalker` for bulk iteration.
//
// Public surface:
//
//   * `query_region`           — single-address MBI query (true on success).
//   * `query_region_status`    — same, returning NTSTATUS.
//   * `query_region_mri`       — single-address MemoryRegionInformationEx
//                                 (allocation extent + Placeholder bit).
//   * `find_alloc_end`         — returns AllocationBase + RegionSize.
//   * `find_alloc_range`       — returns [AllocationBase, AllocationEnd).
//   * `is_range_free`          — true iff every page in the range is MEM_FREE.
//   * `for_committed_batched`  — batched NtSetInformationVirtualMemory over
//                                 committed regions in a span.
//   * `prefetch_committed`     — VmPrefetchInformation with default flags.
//   * `prefetch_materialize`   — VmPrefetchInformation with TO_WORKING_SET.
//   * `prefetch_and_clean_write_watch` — pre-fault + reset write-watch.
//   * `bulk_query_regions`     — low-level NtPssCaptureVaSpaceBulk wrapper.
//   * `RegionWalker`           — iterator over MBI entries in a span.
//
// Each function wraps a single NT syscall (or composes a few). The
// kernel's internal VAD lock makes each individually atomic.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_QUERY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_QUERY_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"  // to_nt_ulong
#include "src/__support/OSUtil/windows/nt_pal/write_watch.h"  // write_watch_reset
#include "src/__support/common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

//===----------------------------------------------------------------------===//
// Single-address queries
//===----------------------------------------------------------------------===//

// Query MBI for a single address. Returns true on success.
LIBC_INLINE bool query_region(const void *addr,
                              MEMORY_BASIC_INFORMATION &mbi) {
  return NT_SUCCESS(nt_helpers::query_basic_info(addr, mbi));
}

// Query MBI returning raw NTSTATUS for callers that need the error code.
LIBC_INLINE NTSTATUS query_region_status(const void *addr,
                                         MEMORY_BASIC_INFORMATION &mbi) {
  return nt_helpers::query_basic_info(addr, mbi);
}

// Query MemoryRegionInformationEx (class 7) for a single address.
// Returns the full allocation extent in `RegionSize` and has the
// `PlaceholderReservation` bit for definitive placeholder detection.
// Fails on MEM_FREE addresses.
LIBC_INLINE bool query_region_mri(const void *addr,
                                  MEMORY_REGION_INFORMATION &mri) {
  SIZE_T ret;
  return NT_SUCCESS(::NtQueryVirtualMemory(NtCurrentProcess(),
                                           const_cast<PVOID>(addr),
                                           MemoryRegionInformationEx, &mri,
                                           sizeof(mri), &ret));
}

// Query MemoryWorkingSetExInformation (class 4) for a contiguous run
// of pages starting at `addr`. The kernel takes an array of
// `MEMORY_WORKING_SET_EX_INFORMATION` entries whose `VirtualAddress`
// fields are pre-populated by the caller (one per page); the kernel
// fills the `VirtualAttributes` in place. Returns true on success.
//
// Used for POSIX `mincore` (page-residency probe) and for NUMA
// `move_pages` / `get_mempolicy` lookups (the `Node` and
// `LargePage`/`Locked` bits are in `VirtualAttributes`).
//
// `count` is the number of entries in `entries[]` (== pages
// queried). Caller pre-fills each `entries[i].VirtualAddress` to
// `addr + i * page_size` (or any specific addresses — the kernel
// queries them independently). The size argument to the syscall is
// `count * sizeof(MEMORY_WORKING_SET_EX_INFORMATION)`.
LIBC_INLINE bool query_working_set_ex(
    MEMORY_WORKING_SET_EX_INFORMATION *entries, size_t count) {
  SIZE_T ret;
  // Class 4 = MemoryWorkingSetExInformation. The kernel ignores the
  // `BaseAddress` parameter for this class (each entry carries its
  // own `VirtualAddress`), so we pass nullptr.
  return NT_SUCCESS(::NtQueryVirtualMemory(
      NtCurrentProcess(), /* BaseAddress= */ nullptr,
      MemoryWorkingSetExInformation, entries,
      count * sizeof(MEMORY_WORKING_SET_EX_INFORMATION), &ret));
}

// Find the end of an allocation via a single MRI query. One syscall,
// O(1). Returns nullptr on query failure (MEM_FREE or invalid).
LIBC_INLINE char *find_alloc_end(const void *addr) {
  MEMORY_REGION_INFORMATION mri;
  if (!query_region_mri(addr, mri))
    return nullptr;
  return static_cast<char *>(mri.AllocationBase) + mri.RegionSize;
}

// Find an allocation's full extent: [alloc_base, alloc_end).
// Single MRI query — O(1).
LIBC_INLINE bool find_alloc_range(const void *addr, char *&out_base,
                                  char *&out_end) {
  MEMORY_REGION_INFORMATION mri;
  if (!query_region_mri(addr, mri))
    return false;
  out_base = static_cast<char *>(mri.AllocationBase);
  out_end = out_base + mri.RegionSize;
  return true;
}

//===----------------------------------------------------------------------===//
// RegionWalker — zero-overhead bulk MBI iterator
//===----------------------------------------------------------------------===//
//
// Replaces per-region NtQueryVirtualMemory loops with bulk queries via
// NtPssCaptureVaSpaceBulk. The caller drives the iteration directly —
// no callbacks, no indirection, no lambda captures.
//
// Range-bounded: walks [start, start+size). The kernel splits MBI
// entries at BaseAddress boundaries, so the first entry starts exactly
// at `start`. `chunk` / `chunk_size` are clamped to the requested range.
//
// Usage:
//   auto ws = byte_scratch(4096);
//   RegionWalker walk(addr, size, ws.data(), ws.size());
//   while (walk.next()) {
//     if (walk.entry->State == MEM_COMMIT)
//       do_work(walk.chunk, walk.chunk_size);
//   }

struct RegionWalker {
  const MEMORY_BASIC_INFORMATION *entry;
  char *chunk;
  SIZE_T chunk_size;

  // Range-bounded walk over [start, start+size).
  LIBC_INLINE RegionWalker(void *start, SIZE_T size, void *buf,
                           SIZE_T buf_size)
      : entry(nullptr), chunk(nullptr), chunk_size(0),
        bulk_(static_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf)),
        buf_size_(buf_size),
        range_start_(static_cast<char *>(start)),
        range_end_(static_cast<char *>(start) + size),
        cursor_(start), entries_(nullptr), count_(0), pos_(0) {}

  // Unbounded walk from `start` (mlockall / munlockall).
  LIBC_INLINE RegionWalker(void *start, NTPSS_MEMORY_BULK_INFORMATION *buf,
                           SIZE_T buf_size)
      : entry(nullptr), chunk(nullptr), chunk_size(0),
        bulk_(buf), buf_size_(buf_size),
        range_start_(static_cast<char *>(start)),
        range_end_(reinterpret_cast<char *>(windows::get_max_address())),
        cursor_(start), entries_(nullptr), count_(0), pos_(0) {}

  LIBC_INLINE bool next() {
    for (;;) {
      while (pos_ < count_) {
        const MEMORY_BASIC_INFORMATION &e = entries_[pos_++];
        char *base = static_cast<char *>(e.BaseAddress);
        char *region_end = base + e.RegionSize;

        if (base >= range_end_)
          return false;

        char *cs = (base > range_start_) ? base : range_start_;
        char *ce = (region_end < range_end_) ? region_end : range_end_;
        if (cs >= ce)
          continue;

        entry = &e;
        chunk = cs;
        chunk_size = static_cast<SIZE_T>(ce - cs);
        return true;
      }

      if (!fetch_page_())
        return false;
    }
  }

private:
  NTPSS_MEMORY_BULK_INFORMATION *bulk_;
  SIZE_T buf_size_;
  char *range_start_;
  char *range_end_;
  PVOID cursor_;
  MEMORY_BASIC_INFORMATION *entries_;
  ULONG count_;
  ULONG pos_;

  LIBC_INLINE bool fetch_page_() {
    if (cursor_ >= range_end_)
      return false;

    bulk_->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
    bulk_->NumberOfEntries = 0;
    bulk_->NextValidAddress = nullptr;

    SIZE_T ret_len = 0;
    NTSTATUS st = ::NtPssCaptureVaSpaceBulk(
        NtCurrentProcess(), cursor_, bulk_, buf_size_, &ret_len);

    if (!NT_SUCCESS(st) && st != STATUS_BUFFER_OVERFLOW)
      return false;

    count_ = bulk_->NumberOfEntries;
    pos_ = 0;
    entries_ = reinterpret_cast<MEMORY_BASIC_INFORMATION *>(bulk_ + 1);

    ULONG max_entries = static_cast<ULONG>(
        (buf_size_ - sizeof(NTPSS_MEMORY_BULK_INFORMATION)) /
        sizeof(MEMORY_BASIC_INFORMATION));
    if (count_ > max_entries)
      count_ = max_entries;

    if (count_ == 0)
      return false;

    PVOID next = bulk_->NextValidAddress;
    if (!next || next <= cursor_)
      cursor_ = range_end_;
    else
      cursor_ = next;

    return true;
  }
};

// Check that the entire [addr, addr+size) range is MEM_FREE.
LIBC_INLINE bool is_range_free(const void *addr, SIZE_T size) {
  auto ws = windows::byte_scratch(4096);
  if (!ws)
    return false;
  RegionWalker walk(const_cast<void *>(addr), size, ws.data(), ws.size());
  while (walk.next()) {
    if (walk.entry->State != MEM_FREE)
      return false;
  }
  return true;
}

// Walk committed regions and batch NtSetInformationVirtualMemory calls.
LIBC_INLINE void for_committed_batched(void *addr, SIZE_T total_size,
                                       VIRTUAL_MEMORY_INFORMATION_CLASS cls,
                                       void *info, SIZE_T info_size) {
  constexpr SIZE_T MAX_BATCH = 64;
  MEMORY_RANGE_ENTRY range_entries[MAX_BATCH];
  SIZE_T batch_count = 0;

  auto ws = windows::byte_scratch(4096);
  if (!ws)
    return;
  RegionWalker walk(addr, total_size, ws.data(), ws.size());
  while (walk.next()) {
    if (walk.entry->State == MEM_COMMIT) {
      range_entries[batch_count].VirtualAddress = walk.chunk;
      range_entries[batch_count].NumberOfBytes = walk.chunk_size;
      ++batch_count;
      if (batch_count == MAX_BATCH) {
        ::NtSetInformationVirtualMemory(NtCurrentProcess(), cls, batch_count,
                                         range_entries, info,
                                         to_nt_ulong(info_size));
        batch_count = 0;
      }
    }
  }

  if (batch_count > 0)
    ::NtSetInformationVirtualMemory(NtCurrentProcess(), cls, batch_count,
                                     range_entries, info,
                                     to_nt_ulong(info_size));
}

// Bulk-query VA space regions starting at `start_addr`. Returns a
// pointer to the MBI array with `count` entries. Low-level — prefer
// `RegionWalker`.
LIBC_INLINE MEMORY_BASIC_INFORMATION *
bulk_query_regions(NTPSS_MEMORY_BULK_INFORMATION *bulk, SIZE_T buf_size,
                   PVOID start_addr, ULONG &count, PVOID &next_addr) {
  bulk->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  bulk->NumberOfEntries = 0;
  bulk->NextValidAddress = nullptr;

  SIZE_T ret_len = 0;
  NTSTATUS st = ::NtPssCaptureVaSpaceBulk(NtCurrentProcess(), start_addr,
                                           bulk, buf_size, &ret_len);
  if (NT_SUCCESS(st) || st == STATUS_BUFFER_OVERFLOW) {
    count = bulk->NumberOfEntries;
    next_addr = bulk->NextValidAddress;
    return reinterpret_cast<MEMORY_BASIC_INFORMATION *>(bulk + 1);
  }
  count = 0;
  next_addr = nullptr;
  return nullptr;
}

// Prefetch committed regions into the working set (caches but does
// not add to WS).
LIBC_INLINE void prefetch_committed(void *addr, SIZE_T total_size) {
  MEMORY_PREFETCH_INFORMATION prefetch = {0};
  for_committed_batched(addr, total_size, VmPrefetchInformation, &prefetch,
                        sizeof(prefetch));
}

// Prefetch + materialize all committed pages into the working set
// (Flags=1). Triggers write-watch dirty on virgin write-watched pages
// — pair with `write_watch_reset` (or use `prefetch_and_clean_write_watch`)
// to establish a clean baseline.
LIBC_INLINE void prefetch_materialize(void *addr, SIZE_T total_size) {
  MEMORY_PREFETCH_INFORMATION prefetch = {VM_PREFETCH_TO_WORKING_SET};
  for_committed_batched(addr, total_size, VmPrefetchInformation, &prefetch,
                        sizeof(prefetch));
}

// Materialize all committed pages, then reset write-watch tracking to
// establish a clean baseline. Two syscalls: prefetch + reset. Use for
// fork() pre-copy on anonymous MAP_PRIVATE and for `MADV_POPULATE_WRITE`
// on write-watched allocations.
LIBC_INLINE void prefetch_and_clean_write_watch(void *addr, SIZE_T size) {
  prefetch_materialize(addr, size);
  write_watch_reset(addr, size);
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_QUERY_H
