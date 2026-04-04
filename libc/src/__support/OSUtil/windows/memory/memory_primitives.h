//===-- Windows memory primitives --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Atomic NT memory operations — one level above raw syscalls, one level
// below POSIX semantics. Everything the mmap subsystem needs to query,
// allocate, commit, decommit, protect, and release virtual memory.
//
// Organized by operation type:
//
//   Query       — query_region, query_region_mri, find_alloc_end,
//                 find_alloc_range, is_range_free, for_committed_batched,
//                 prefetch_committed
//   Placeholder — create, create_ex, create_32bit, create_numa,
//                 split, release, preserve_to, replace_commit
//   Commit      — vm_commit, vm_decommit, vm_dontneed
//   Release     — vm_release
//   Partial     — interior_release, decommit_private_range
//                 (used by partial-munmap paths on placeholder-committed VA
//                 and by the table-miss fallback for foreign MEM_PRIVATE VA)
//   WriteWatch  — write_watch_query, write_watch_query_reset,
//                 write_watch_reset
//   Reset       — vm_reset
//   Coalesce    — vm_coalesce_placeholders
//
// Each function wraps a single NT syscall. The kernel's internal VAD lock
// makes each individually atomic. No userspace synchronization.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_PRIMITIVES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_PRIMITIVES_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

LIBC_INLINE ULONG nt_ulong(SIZE_T value) {
  LIBC_ASSERT(value <= static_cast<SIZE_T>(~static_cast<ULONG>(0)) &&
              "NT size argument exceeds ULONG range");
  return static_cast<ULONG>(value);
}

//===----------------------------------------------------------------------===//
// Query
//===----------------------------------------------------------------------===//

/// Query MBI for a single address. Returns true on success.
LIBC_INLINE bool query_region(const void *addr,
                              MEMORY_BASIC_INFORMATION &mbi) {
  return NT_SUCCESS(nt_helpers::query_basic_info(addr, mbi));
}

/// Query MBI returning raw NTSTATUS for callers that need the error code.
LIBC_INLINE NTSTATUS query_region_status(const void *addr,
                                         MEMORY_BASIC_INFORMATION &mbi) {
  return nt_helpers::query_basic_info(addr, mbi);
}

/// Query MemoryRegionInformationEx (class 7) for a single address.
/// Returns the full allocation extent in RegionSize and has the
/// PlaceholderReservation bit for definitive placeholder detection.
/// Returns true on success. Fails on MEM_FREE addresses.
LIBC_INLINE bool query_region_mri(const void *addr,
                                  MEMORY_REGION_INFORMATION &mri) {
  SIZE_T ret;
  return NT_SUCCESS(::NtQueryVirtualMemory(NtCurrentProcess(),
                                           const_cast<PVOID>(addr),
                                           MemoryRegionInformationEx, &mri,
                                           sizeof(mri), &ret));
}

/// Find the end of an allocation via a single MRI query.
/// Returns (AllocationBase + RegionSize) — the first address past the
/// allocation. One syscall, O(1), replaces the old RegionWalker walk.
/// Returns nullptr on query failure (MEM_FREE or invalid address).
LIBC_INLINE char *find_alloc_end(const void *addr) {
  MEMORY_REGION_INFORMATION mri;
  if (!query_region_mri(addr, mri))
    return nullptr;
  return static_cast<char *>(mri.AllocationBase) + mri.RegionSize;
}

/// Find an allocation's full extent: [alloc_base, alloc_end).
/// Returns false if the query fails or the address is MEM_FREE.
/// Single MRI query — O(1).
LIBC_INLINE bool find_alloc_range(const void *addr,
                                  char *&out_base, char *&out_end) {
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
// Range-bounded: walks [start, start+size). The kernel splits MBI entries
// at BaseAddress boundaries, so the first entry starts exactly at `start`.
// chunk/chunk_size are clamped to the requested range.
//
// Usage:
//   auto ws = byte_scratch(4096);
//   RegionWalker walk(addr, size, ws.data(), ws.size());
//   while (walk.next()) {
//     if (walk.entry->State == MEM_COMMIT)
//       do_work(walk.chunk, walk.chunk_size);
//   }
//
// Unbounded (full VA space, e.g. mlockall): pass a heap-allocated buffer
// via the NTPSS_MEMORY_BULK_INFORMATION* constructor overload.

struct RegionWalker {
  /// Current MBI entry — points into the bulk buffer.
  /// Valid after next() returns true, until the next next() call.
  const MEMORY_BASIC_INFORMATION *entry;

  /// Clamped chunk within the requested range.
  char *chunk;
  SIZE_T chunk_size;

  /// Range-bounded walk over [start, start+size).
  LIBC_INLINE RegionWalker(void *start, SIZE_T size, void *buf,
                           SIZE_T buf_size)
      : entry(nullptr), chunk(nullptr), chunk_size(0),
        bulk_(static_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf)),
        buf_size_(buf_size),
        range_start_(static_cast<char *>(start)),
        range_end_(static_cast<char *>(start) + size),
        cursor_(start), entries_(nullptr), count_(0), pos_(0) {}

  /// Unbounded walk from `start` (for mlockall/munlockall).
  /// range_end_ set to max user VA; caller stops when next() returns false.
  LIBC_INLINE RegionWalker(void *start, NTPSS_MEMORY_BULK_INFORMATION *buf,
                           SIZE_T buf_size)
      : entry(nullptr), chunk(nullptr), chunk_size(0),
        bulk_(buf), buf_size_(buf_size),
        range_start_(static_cast<char *>(start)),
        range_end_(reinterpret_cast<char *>(get_max_address())),
        cursor_(start), entries_(nullptr), count_(0), pos_(0) {}

  /// Advance to the next region. Returns false when done.
  LIBC_INLINE bool next() {
    for (;;) {
      // Consume entries from the current page.
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

      // Fetch next page of bulk results.
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

    // Defensive: clamp to what physically fits in the buffer.
    ULONG max_entries = static_cast<ULONG>(
        (buf_size_ - sizeof(NTPSS_MEMORY_BULK_INFORMATION)) /
        sizeof(MEMORY_BASIC_INFORMATION));
    if (count_ > max_entries)
      count_ = max_entries;

    if (count_ == 0)
      return false;

    // Advance cursor for next page.
    PVOID next = bulk_->NextValidAddress;
    if (!next || next <= cursor_)
      cursor_ = range_end_; // done after this page
    else
      cursor_ = next;

    return true;
  }
};

/// Check that the entire [addr, addr+size) range is MEM_FREE.
LIBC_INLINE bool is_range_free(const void *addr, SIZE_T size) {
  auto ws = byte_scratch(4096);
  if (!ws) return false;
  RegionWalker walk(const_cast<void *>(addr), size, ws.data(), ws.size());
  while (walk.next()) {
    if (walk.entry->State != MEM_FREE)
      return false;
  }
  return true;
}

/// Walk committed regions and batch NtSetInformationVirtualMemory calls.
LIBC_INLINE void for_committed_batched(void *addr, SIZE_T total_size,
                                       VIRTUAL_MEMORY_INFORMATION_CLASS cls,
                                       void *info, SIZE_T info_size) {
  constexpr SIZE_T MAX_BATCH = 64;
  MEMORY_RANGE_ENTRY range_entries[MAX_BATCH];
  SIZE_T batch_count = 0;

  auto ws = byte_scratch(4096);
  if (!ws) return;
  RegionWalker walk(addr, total_size, ws.data(), ws.size());
  while (walk.next()) {
    if (walk.entry->State == MEM_COMMIT) {
      range_entries[batch_count].VirtualAddress = walk.chunk;
      range_entries[batch_count].NumberOfBytes = walk.chunk_size;
      ++batch_count;
      if (batch_count == MAX_BATCH) {
        ::NtSetInformationVirtualMemory(NtCurrentProcess(), cls, batch_count,
                                        range_entries, info,
                                        nt_ulong(info_size));
        batch_count = 0;
      }
    }
  }

  if (batch_count > 0)
    ::NtSetInformationVirtualMemory(NtCurrentProcess(), cls, batch_count,
                                    range_entries, info, nt_ulong(info_size));
}

/// Bulk-query VA space regions starting at `start_addr`. Returns a pointer
/// to the MBI array with `count` entries. Low-level — prefer RegionWalker.
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

/// Prefetch committed regions into the working set.
LIBC_INLINE void prefetch_committed(void *addr, SIZE_T total_size) {
  MEMORY_PREFETCH_INFORMATION prefetch = {0};
  for_committed_batched(addr, total_size, VmPrefetchInformation,
                        &prefetch, sizeof(prefetch));
}

/// Prefetch committed regions directly into the working set (Flags=1).
/// Unlike prefetch_committed (Flags=0, which caches but doesn't add to WS),
/// this materializes all pages as resident. One syscall batch per 64 regions.
/// NOTE: triggers write-watch dirty on virgin write-watched pages.
// Forward declaration — defined below in the Write-Watch section.
LIBC_INLINE bool write_watch_reset(void *addr, SIZE_T size);

/// Use prefetch_and_clean_write_watch() for write-watched allocations.
LIBC_INLINE void prefetch_materialize(void *addr, SIZE_T total_size) {
  MEMORY_PREFETCH_INFORMATION prefetch = {VM_PREFETCH_TO_WORKING_SET};
  for_committed_batched(addr, total_size, VmPrefetchInformation,
                        &prefetch, sizeof(prefetch));
}

/// Materialize all committed pages into the working set, then reset
/// write-watch tracking to establish a clean baseline. After this call,
/// only genuine user writes will appear dirty — demand-zero and prefetch
/// false positives are eliminated.
///
/// Two syscalls: VmPrefetchInformation(Flags=1) + NtResetWriteWatch.
/// Use for fork() pre-copy (anonymous MAP_PRIVATE) and madvise
/// MADV_POPULATE_WRITE on write-watched allocations.
LIBC_INLINE void prefetch_and_clean_write_watch(void *addr, SIZE_T size) {
  prefetch_materialize(addr, size);
  write_watch_reset(addr, size);
}

//===----------------------------------------------------------------------===//
// Placeholder — Create
//===----------------------------------------------------------------------===//

/// Reserve a placeholder at addr (or system-chosen if nullptr).
LIBC_INLINE void *create_placeholder(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (!NT_SUCCESS(st))
    return nullptr;
  return base;
}

/// Reserve a placeholder, returning the kernel-rounded actual size.
LIBC_INLINE void *create_placeholder_ex(void *addr, SIZE_T size,
                                        SIZE_T &actual_size) {
  PVOID base = addr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (NT_SUCCESS(st)) {
    actual_size = actual;
    return base;
  }
  actual_size = 0;
  return nullptr;
}

/// Reserve a placeholder constrained to the low 2 GB (MAP_32BIT).
LIBC_INLINE void *create_placeholder_32bit(SIZE_T size) {
  MEM_ADDRESS_REQUIREMENTS reqs = {};
  reqs.HighestEndingAddress =
      reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(0x7FFFFFFFU));

  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterAddressRequirements;
  param.Pointer = &reqs;

  PVOID base = nullptr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
  return NT_SUCCESS(st) ? base : nullptr;
}

/// Create a placeholder with NUMA node affinity.
LIBC_INLINE void *create_placeholder_numa(void *addr, SIZE_T size,
                                          ULONG numa_node) {
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = numa_node;

  PVOID base = addr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
  return NT_SUCCESS(st) ? base : nullptr;
}

//===----------------------------------------------------------------------===//
// Placeholder — Split / Release
//===----------------------------------------------------------------------===//

/// Split a placeholder at split_offset into two independent placeholders.
LIBC_INLINE bool split_placeholder(void *addr, SIZE_T split_offset) {
  PVOID base = addr;
  SIZE_T size = split_offset;
  return NT_SUCCESS(::NtFreeVirtualMemory(
      NtCurrentProcess(), &base, &size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
}

/// Release a placeholder entirely, returning the VA to MEM_FREE.
LIBC_INLINE bool release_placeholder(void *addr) {
  PVOID base = addr;
  SIZE_T region_size = 0;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &region_size,
                            MEM_RELEASE));
}

//===----------------------------------------------------------------------===//
// Section view — Unmap
//===----------------------------------------------------------------------===//

/// Tear down a section view, leaving the VA as MEM_FREE.
/// Use when the view's address space is being released (full munmap, error
/// rollback that does not need to preserve the placeholder).
LIBC_INLINE NTSTATUS unmap_view(void *addr) {
  return ::NtUnmapViewOfSectionEx(NtCurrentProcess(), addr, 0);
}

/// Tear down a section view but keep the VA reserved as a placeholder.
/// Use during split / rollback paths where the placeholder will be reused
/// or coalesced. Mirrors the kernel's MEM_PRESERVE_PLACEHOLDER convention.
LIBC_INLINE NTSTATUS unmap_view_preserve(void *addr) {
  return ::NtUnmapViewOfSectionEx(NtCurrentProcess(), addr,
                                  MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
}

/// Same as unmap_view_preserve but with the kernel's transient priority
/// boost (cheap deferred-IO hint). Use on hot paths where the unmap is
/// the dominant cost. Pure perf shape — no semantic difference.
LIBC_INLINE NTSTATUS unmap_view_preserve_transient(void *addr) {
  return ::NtUnmapViewOfSectionEx(
      NtCurrentProcess(), addr,
      MEM_UNMAP_WITH_TRANSIENT_BOOST | MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
}

//===----------------------------------------------------------------------===//
// Placeholder — Atomic State Transitions (Kernel CAS)
//===----------------------------------------------------------------------===//

/// committed → placeholder. Returns NTSTATUS for conflict detection.
LIBC_INLINE NTSTATUS preserve_to_placeholder(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

/// placeholder → committed. Returns NTSTATUS for conflict detection.
LIBC_INLINE NTSTATUS replace_placeholder_commit(void *addr, SIZE_T size,
                                                DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, page_prot, nullptr,
      0);
}

/// placeholder → reserved (no commit). Returns NTSTATUS.
/// Used for MAP_NORESERVE demand-commit via VEH. Pages are committed
/// on demand by the VEH fault handler reading AllocationProtect from MBI.
LIBC_INLINE NTSTATUS replace_placeholder_reserve(void *addr, SIZE_T size,
                                                 DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, page_prot, nullptr, 0);
}

//===----------------------------------------------------------------------===//
// Commit / Decommit
//===----------------------------------------------------------------------===//

/// Commit pages within an existing reservation. Zero-fill on first touch.
/// Works on both normal MEM_RESERVE and ex-placeholder MEM_PRIVATE regions.
LIBC_INLINE NTSTATUS vm_commit(void *addr, SIZE_T size, DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz,
                                     MEM_COMMIT, page_prot, nullptr, 0);
}

/// Decommit pages — frees physical memory and commit charge.
/// Pages become MEM_RESERVE (inaccessible). Surrounding committed
/// pages within the same allocation are untouched.
LIBC_INLINE bool vm_decommit(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_DECOMMIT));
}

/// MADV_DONTNEED for private memory: decommit + recommit.
/// Guarantees zero-fill, frees commit charge transiently.
/// Preserves the original page protection via \p prot.
LIBC_INLINE void vm_dontneed(void *addr, SIZE_T size, DWORD prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_DECOMMIT);
  base = addr;
  sz = size;
  ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz, MEM_COMMIT,
                               prot, nullptr, 0);
}

//===----------------------------------------------------------------------===//
// Release
//===----------------------------------------------------------------------===//

/// Release an entire allocation (size=0) or a sub-range.
/// For sub-ranges, the region must be decommitted first.
LIBC_INLINE bool vm_release(void *addr, SIZE_T size = 0) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_RELEASE));
}

//===----------------------------------------------------------------------===//
// Partial — sub-range release/decommit primitives
//===----------------------------------------------------------------------===//

/// Release an interior sub-range of a MEM_PRIVATE allocation to MEM_FREE.
///
/// Works on committed or decommitted sub-ranges, at any position (head,
/// tail, interior), with page granularity (4KB). The freed VA becomes
/// MEM_FREE and is reclaimable by future allocations. Surrounding
/// committed data within the parent allocation survives — each surviving
/// fragment becomes an independent allocation with its own AllocationBase.
///
/// Used by (a) mremap shrink on placeholder-committed MEM_PRIVATE VA, and
/// (b) the vm_protect table-miss fallback for foreign MEM_PRIVATE VA.
///
/// Returns true on success, false on failure.
LIBC_INLINE bool interior_release(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_RELEASE));
}

/// Decommit a sub-range of a MEM_PRIVATE placeholder-committed allocation.
///
/// Single NtFreeVirtualMemory(MEM_DECOMMIT) call. The sub-range becomes
/// MEM_RESERVE (inaccessible, faults on access — correct munmap semantics)
/// while the enclosing allocation stays intact: surrounding committed pages
/// keep the same AllocationBase, and the decommitted sub-range is ready for
/// in-place anonymous MAP_FIXED recommit via a single vm_commit() call
/// (zero-filled on recommit).
///
/// This is the partial-munmap primitive for ANON_PLACEHOLDER regions with
/// region_flag::COMMITTED. For PROT_NONE / NORESERVE placeholders use
/// split_placeholder + release instead.
///
/// On MEM_WRITE_WATCH allocations, MEM_DECOMMIT clears dirty tracking
/// for the range (see WriteWatch interaction rules above).
LIBC_INLINE bool decommit_private_range(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_DECOMMIT));
}

//===----------------------------------------------------------------------===//
// Write-Watch — Hardware-assisted dirty page tracking
//===----------------------------------------------------------------------===//
//
// MEM_WRITE_WATCH enables per-page dirty tracking on MEM_PRIVATE regions
// at zero additional syscall cost — the flag is added to the existing
// NtAllocateVirtualMemoryEx commit call. The kernel maintains a bitmap
// (1 bit per 4KB page) updated by the hardware PTE dirty mechanism.
//
// Query cost scales linearly with region size (~16 cycles/page), not
// dirty page count. Atomic query+reset (WRITE_WATCH_FLAG_RESET)
// eliminates race windows between query and reset.
//
// Interaction rules (from RA17-22):
//   TRIGGERS dirty:  user writes, NtReadFile, IoRing READ (unregistered),
//                    NtLockVirtualMemory (virgin), VmPrefetch(TO_WS, virgin)
//   CLEARS dirty:   MEM_DECOMMIT, MEM_RESET, NtGetWriteWatch(RESET),
//                    NtResetWriteWatch
//   NO EFFECT:      NtProtectVirtualMemory, VmRemoveFromWS, IoRing WRITE,
//                    IoRing READ (registered buffer — MDL bypass)
//
// Not available on section views (MEM_MAPPED) — only MEM_PRIVATE.
// Incompatible with MEM_LARGE_PAGES and MEM_RESERVE_PLACEHOLDER
// (use two-step: reserve placeholder, then replace with WRITE_WATCH).

/// Query dirty pages without clearing. Returns the number of dirty page
/// addresses written to the output array. The array must have capacity
/// for at least (region_size / PAGE_SIZE) entries for complete results;
/// if undersized, the kernel silently truncates (no overflow error).
///
/// Granularity is always 4096 (page size) on current NT kernels.
/// Returns 0 on failure (region not allocated with MEM_WRITE_WATCH,
/// or other error).
LIBC_INLINE SIZE_T write_watch_query(void *addr, SIZE_T size,
                                     void **page_addrs,
                                     ULONG_PTR max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st =
      ::NtGetWriteWatch(NtCurrentProcess(), 0, addr, size, page_addrs, &count,
                        &granularity);
  return NT_SUCCESS(st) ? static_cast<SIZE_T>(count) : 0;
}

/// Query dirty pages AND atomically reset the tracking bitmap.
/// After this call, only pages written after the reset will appear
/// in subsequent queries. No race window — any write that lands during
/// the syscall is either captured in the returned set or tracked in
/// the new epoch.
///
/// This is the preferred API for incremental dirty tracking (CoW
/// preservation, fork optimization, msync).
LIBC_INLINE SIZE_T write_watch_query_reset(void *addr, SIZE_T size,
                                           void **page_addrs,
                                           ULONG_PTR max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st = ::NtGetWriteWatch(NtCurrentProcess(), WRITE_WATCH_FLAG_RESET,
                                  addr, size, page_addrs, &count, &granularity);
  return NT_SUCCESS(st) ? static_cast<SIZE_T>(count) : 0;
}

/// Cross-process write-watch query (RA19.4). Uses an explicit process
/// handle instead of NtCurrentProcess(). Enables a parent process to query
/// a child's dirty pages after fork — the parent holds PROCESS_VM_READ |
/// PROCESS_VM_OPERATION on the child. Proven on Windows 11 26200.
LIBC_INLINE SIZE_T write_watch_query_remote(HANDLE process, void *addr,
                                            SIZE_T size, void **page_addrs,
                                            ULONG_PTR max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st =
      ::NtGetWriteWatch(process, 0, addr, size, page_addrs, &count,
                        &granularity);
  return NT_SUCCESS(st) ? static_cast<SIZE_T>(count) : 0;
}

/// Cross-process write-watch query+reset (RA19.4). Atomically queries
/// and resets dirty tracking on a remote process's write-watched pages.
LIBC_INLINE SIZE_T write_watch_query_reset_remote(HANDLE process, void *addr,
                                                  SIZE_T size,
                                                  void **page_addrs,
                                                  ULONG_PTR max_entries) {
  ULONG_PTR count = max_entries;
  ULONG granularity = 0;
  NTSTATUS st = ::NtGetWriteWatch(process, WRITE_WATCH_FLAG_RESET,
                                  addr, size, page_addrs, &count, &granularity);
  return NT_SUCCESS(st) ? static_cast<SIZE_T>(count) : 0;
}

/// Reset write-watch tracking for a sub-range without querying.
/// After this call, only new writes to the range will appear dirty.
/// Supports arbitrary page-aligned sub-ranges within the allocation.
///
/// Use case: after bulk pre-fault (VmPrefetchInformation TO_WORKING_SET)
/// or IoRing buffer registration, reset to establish a clean baseline
/// where only genuine user writes appear dirty.
LIBC_INLINE bool write_watch_reset(void *addr, SIZE_T size) {
  return NT_SUCCESS(::NtResetWriteWatch(NtCurrentProcess(), addr, size));
}

//===----------------------------------------------------------------------===//
// Reset / Coalesce
//===----------------------------------------------------------------------===//

/// Mark pages as discardable (MADV_FREE equivalent). Content may survive
/// or be zero-filled on next access — no guarantee either way.
LIBC_INLINE void vm_reset(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz, MEM_RESET,
                               PAGE_READWRITE, nullptr, 0);
}

/// Attempt to cancel a prior vm_reset() (MADV_FREE undo).
/// Returns true if all pages still contain their original content.
/// Returns false if any page was already reclaimed by the kernel (zeroed).
/// Safe to call speculatively on non-reset pages: the kernel checks the
/// reset bit per PTE and treats non-reset pages as a no-op, returning
/// success. This makes state tracking unnecessary.
LIBC_INLINE bool vm_reset_undo(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base,
                                                &sz, MEM_RESET_UNDO,
                                                PAGE_READWRITE, nullptr, 0));
}

/// Coalesce adjacent placeholders into a single contiguous placeholder.
LIBC_INLINE NTSTATUS vm_coalesce_placeholders(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz,
                               MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_PRIMITIVES_H
