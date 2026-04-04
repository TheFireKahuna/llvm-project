//===---------- Windows madvise engine (kernel function) -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX madvise via Windows memory APIs.
//
// MADV_DONTNEED:
//   Linux guarantees zero-fill on next access for anonymous pages.
//   Anonymous: decommit + recommit provides the same guarantee. The full
//   protection (including modifier flags like PAGE_GUARD) is preserved
//   across the cycle.
//   File-backed: NtAllocateVirtualMemoryEx(MEM_RESET) marks pages as
//   discardable. The memory manager reclaims them when physical frames are
//   needed; subsequent access re-faults from the backing file. MEM_RESET on
//   mapped views is rejected by the Win32 VirtualAlloc wrapper but accepted
//   by the NT syscall. After marking pages discardable, we demote their
//   priority to MEMORY_PRIORITY_LOWEST and evict from the working set so
//   they are the first to be reclaimed under any memory pressure.
//
// MADV_FREE:
//   MEM_RESET marks pages as discardable while preserving content until the
//   memory manager needs the frames -- matching Linux's lazy reclaim semantic.
//   EINVAL for file-backed (matching Linux).
//
// MADV_WILLNEED:
//   MEM_RESET_UNDO first on MEM_PRIVATE regions: if a prior MADV_FREE marked
//   pages discardable and the kernel hasn't reclaimed them yet, this recovers
//   the original content at zero cost -- matching Linux's MADV_FREE->access
//   round-trip. Falls through to PrefetchVirtualMemory regardless, which
//   warms the working set whether or not content was recovered.
//
// MADV_SEQUENTIAL / MADV_RANDOM / MADV_NORMAL:
//   Page eviction priority hints via VmPagePriorityInformation. Sequential
//   access patterns benefit from aggressive eviction (LOW priority) since
//   pages behind the read pointer won't be revisited. Random access keeps
//   pages longer (BELOW_NORMAL) to exploit temporal locality.
//
// MADV_COLD:
//   Data-preserving priority demotion. No MEM_RESET.
//
// MADV_PAGEOUT:
//   Priority demotion + working set eviction. MEM_RESET applied only to
//   shared/read-only file mappings (safe -- content is in the file). Private
//   COW mappings (PAGE_WRITECOPY) and anonymous pages are preserved.
//
// MADV_POPULATE_READ:
//   Pre-fault pages via PrefetchVirtualMemory.
//
// MADV_POPULATE_WRITE:
//   PrefetchVirtualMemory for bulk read-prefetch, then per-page volatile
//   write-back on PAGE_WRITECOPY regions to trigger copy-on-write. This
//   materializes private COW copies so subsequent writes avoid fault latency.
//   Returns EFAULT for non-writable pages (matches Linux).
//
// MADV_DONTDUMP / MADV_DODUMP:
//   WER crash dump exclusion via direct PEB gather list manipulation.
//   WerFault.exe reads this cross-process at crash time. No NT syscall.
//
// MADV_HUGEPAGE / MADV_NOHUGEPAGE / MADV_MERGEABLE / MADV_UNMERGEABLE:
//   No Windows equivalent; succeed silently (advisory).
//
//===----------------------------------------------------------------------===//

#include "madvise_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/memory_region.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_utils.h"

#include <stdint.h> // UINTPTR_MAX

namespace LIBC_NAMESPACE_DECL {

namespace {

//==========================================================================
// WER crash dump exclusion (MADV_DONTDUMP / MADV_DODUMP)
//==========================================================================
//
// Directly manipulates the PEB WER gather list. Each DONTDUMP call prepends
// a WER_GATHER node with the exclusion flag (bit 15). DODUMP walks the list
// and unlinks matching nodes. WerFault.exe reads this list cross-process at
// crash time -- no NT syscall involved.
//
// Nodes are allocated from a static pool (no heap dependency). The pool is
// protected by a simple spinlock since DONTDUMP/DODUMP are infrequent.

/// Retrieve the WER registration header from the PEB.
WER_PEB_HEADER_BLOCK *WerGetPebHeader() {
  return NtCurrentPeb()->WerRegistrationData;
}

Futex g_wer_lock{0};

/// Slab for WER_GATHER nodes. Committed on first DONTDUMP call.
/// A single page (4KB) holds ~128 nodes -- sufficient for any realistic
/// DONTDUMP usage (ASan typically registers one per shadow region).
char *g_wer_slab = nullptr;
SIZE_T g_wer_used = 0;
SIZE_T g_wer_cap = 0;

void wer_lock() {
  for (;;) {
    FutexValueType expected = 0;
    if (g_wer_lock.compare_exchange_weak(expected, 1,
                                         cpp::MemoryOrder::ACQUIRE,
                                         cpp::MemoryOrder::RELAXED))
      return;
    // Yield on transient wait failure (-ENOMEM from wait-slot pool
    // exhaustion) rather than spin-fail through wait() hot. The WER
    // lock serves hdr_madvise's per-process error-reporting scratch;
    // no caller context can handle an acquire failure, so yielding
    // until another thread releases the lock is the only sound move.
    long ret = g_wer_lock.wait(1);
    if (ret < 0 && ret != -EINTR)
      ::NtYieldExecution();
  }
}

void wer_unlock() {
  g_wer_lock.store_and_notify(0);
}

/// Bump-allocate a WER_GATHER node. Called under wer_lock.
WER_GATHER *alloc_gather_node() {
  constexpr SIZE_T NODE_SIZE = sizeof(WER_GATHER);

  if (g_wer_used + NODE_SIZE > g_wer_cap) {
    SIZE_T page = windows::get_page_size();
    windows::PlaceholderRange ph = windows::PlaceholderRange::reserve(page);
    if (!ph)
      return nullptr;
    void *base = ph.base();
    if (NT_ERROR(ph.commit(PAGE_READWRITE)))
      return nullptr; // ~ph releases placeholder on failure.
    g_wer_slab = static_cast<char *>(base);
    g_wer_used = 0;
    g_wer_cap = page;
  }

  WER_GATHER *node =
      reinterpret_cast<WER_GATHER *>(g_wer_slab + g_wer_used);
  g_wer_used += NODE_SIZE;
  return node;
}

/// Register a memory range for exclusion from WER crash dumps.
int wer_exclude(void *addr, SIZE_T size) {
  WER_PEB_HEADER_BLOCK *hdr = WerGetPebHeader();
  if (!hdr)
    return 0; // WER not initialized -- no dump writer will read it.

  WER_GATHER *node = alloc_gather_node();
  if (!node)
    return 0; // Pool full -- advisory, don't fail.

  node->v.Memory.Address = addr;
  node->v.Memory.Size = static_cast<ULONG>(size > 0xFFFFFFFFU ? 0xFFFFFFFFU
                                                                : size);
  node->Flags = WER_GATHER_FLAG_MEMORY | WER_GATHER_FLAG_EXCLUDE;

  wer_lock();
  node->Next = hdr->Gather;
  hdr->Gather = node;
  ++hdr->GatherCount;
  wer_unlock();
  return 0;
}

/// Remove a previously excluded range (MADV_DODUMP).
int wer_include(void *addr) {
  WER_PEB_HEADER_BLOCK *hdr = WerGetPebHeader();
  if (!hdr)
    return 0;

  wer_lock();
  WER_GATHER **pp = &hdr->Gather;
  while (*pp) {
    WER_GATHER *cur = *pp;
    if ((cur->Flags & WER_GATHER_FLAG_EXCLUDE) &&
        cur->v.Memory.Address == addr) {
      *pp = static_cast<WER_GATHER *>(cur->Next);
      --hdr->GatherCount;
      // Node remains in the static pool -- not reusable, but DODUMP is rare.
      break;
    }
    pp = reinterpret_cast<WER_GATHER **>(&cur->Next);
  }
  wer_unlock();
  return 0;
}

} // namespace

namespace internal {

intptr_t madvise(void *addr, size_t size, int advice) {
  // Linux returns ENOMEM for NULL (unmapped address), not EINVAL.
  if (LIBC_UNLIKELY(!addr))
    return -ENOMEM;

  if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
    return -EINVAL;

  if (size == 0)
    return 0;

  const SIZE_T rounded_size = windows::round_to_page(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -ENOMEM;

  // Guard against pointer overflow before entering the region loop.
  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(addr_val > UINTPTR_MAX - rounded_size))
    return -ENOMEM;

  HANDLE current_process = NtCurrentProcess();

  switch (advice) {
  //==========================================================================
  // MADV_DONTNEED: Discard page contents (zeroed on next access)
  //==========================================================================
  // Linux semantics: immediately destroys anonymous page contents; next
  // access returns zero-filled pages. File-backed MAP_PRIVATE pages revert
  // to original file content.
  //
  // Tiered by memory type:
  //   MEM_PRIVATE: decommit + recommit (zero-fill, frees commit charge)
  //   MEM_MAPPED + file WRITECOPY: PAGE_REVERT_TO_FILE_MAP (restores file)
  //   MEM_MAPPED (other): WSEX-selective memset + MEM_RESET (2x faster
  //     than DiscardVirtualMemory, works on section views)
  case MADV_DONTNEED: {
    // All mutations (decommit+recommit, revert-to-file, wsex-dontneed) act
    // on the current chunk only. Subsequent bulk entries remain valid because
    // per-chunk mutations don't change adjacent regions' boundaries or type.
    auto ws = windows::byte_scratch(4096);
    if (!ws) return -ENOMEM;
    windows::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State != MEM_COMMIT)
        continue;

      if (walk.entry->Type == MEM_PRIVATE) {
        windows::vm_dontneed(walk.chunk, walk.chunk_size,
                             walk.entry->AllocationProtect);
      } else if (walk.entry->Type == MEM_MAPPED) {
        bool is_writecopy =
            walk.entry->AllocationProtect == PAGE_WRITECOPY ||
            walk.entry->AllocationProtect == PAGE_EXECUTE_WRITECOPY;
        windows::SlotSnapshot snap = {};
        bool have_snap =
            windows::g_mapping_table.snapshot(
                walk.entry->AllocationBase, &snap);
        const bool file_backed =
            have_snap && snap.region != nullptr &&
            snap.region->file_handle != nullptr;
        const bool noreserve =
            have_snap && snap.region != nullptr &&
            snap.region->has_flag(windows::memory::region_flag::NORESERVE);
        if (is_writecopy && file_backed) {
          PVOID base = walk.chunk;
          SIZE_T sz = walk.chunk_size;
          ULONG old_prot;
          ::NtProtectVirtualMemory(
              current_process, &base, &sz,
              PAGE_WRITECOPY | PAGE_REVERT_TO_FILE_MAP, &old_prot);
        } else if (noreserve) {
          windows::vm_decommit(walk.chunk, walk.chunk_size);
        } else {
          windows::wsex_dontneed_section_pages(walk.chunk, walk.chunk_size);
        }
      }
    }
    return 0;
  }

  //==========================================================================
  // MADV_FREE: Best-effort lazy page discard
  //==========================================================================
  // Linux: content preserved until the system reclaims under memory pressure.
  // On reclaim, pages are discarded (not paged to swap). Restricted to
  // private anonymous VMAs; returns EINVAL for file-backed (matching Linux).
  //
  // Implementation: VmRemoveFromWorkingSetInformation WITHOUT mark-clean.
  // Pages are evicted from the working set but remain dirty -- the kernel
  // may page them to swap or discard them under pressure. This matches
  // MADV_FREE's lazy semantics: content survives if no pressure, lost if
  // reclaimed. MEM_PRIVATE regions use MEM_RESET (equivalent semantics).
  case MADV_FREE: {
    auto ws = windows::byte_scratch(4096);
    if (!ws) return -ENOMEM;
    windows::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State != MEM_COMMIT)
        continue;
      if (walk.entry->Type == MEM_IMAGE)
        return -EINVAL;
      if (walk.entry->Type == MEM_MAPPED) {
        windows::SlotSnapshot snap = {};
        if (windows::g_mapping_table.snapshot(
                walk.entry->AllocationBase, &snap) &&
            snap.region != nullptr &&
            snap.region->file_handle != nullptr)
          return -EINVAL;
        MEMORY_RANGE_ENTRY range;
        range.VirtualAddress = walk.chunk;
        range.NumberOfBytes = walk.chunk_size;
        // Flags=1: ~27% faster than Flags=0 with identical behavior (RA16.4).
        MEMORY_REMOVE_WORKING_SET_INFORMATION rm = {1};
        ::NtSetInformationVirtualMemory(
            current_process, VmRemoveFromWorkingSetInformation, 1, &range,
            &rm, sizeof(rm));
      } else if (walk.entry->Type == MEM_PRIVATE) {
        windows::vm_reset(walk.chunk, walk.chunk_size);
      }
    }
    return 0;
  }

  //==========================================================================
  // MADV_WILLNEED: Cancel prior MADV_FREE, then prefetch into working set
  //==========================================================================
  case MADV_WILLNEED: {
    // vm_reset_undo and vm_commit mutate the current chunk only -- adjacent
    // regions are unaffected, so bulk entries remain valid for cursor advance.
    auto ws = windows::byte_scratch(4096);
    if (!ws) return -ENOMEM;
    windows::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_COMMIT && walk.entry->Type == MEM_PRIVATE)
        windows::vm_reset_undo(walk.chunk, walk.chunk_size);

      if (walk.entry->State == MEM_RESERVE && walk.entry->Type == MEM_MAPPED) {
        windows::SlotSnapshot snap = {};
        if (windows::g_mapping_table.snapshot(
                walk.entry->AllocationBase, &snap) &&
            snap.region != nullptr &&
            snap.region->has_flag(windows::memory::region_flag::NORESERVE)) {
          windows::vm_commit(walk.chunk, walk.chunk_size, snap.view_prot);
        }
      }
    }
    windows::prefetch_committed(addr, rounded_size);
    return 0;
  }

  //==========================================================================
  // Access pattern hints via page eviction priority
  //==========================================================================
  // Windows lacks direct readahead/eviction-pattern controls, but page
  // priority influences eviction order under memory pressure:
  //   SEQUENTIAL (LOW): pages behind the read pointer won't be revisited,
  //     so evict them sooner to free frames for the read-ahead window.
  //   RANDOM (BELOW_NORMAL): temporal locality means recently-touched pages
  //     may be revisited, so keep them slightly longer.
  //   NORMAL: restore default eviction behavior.
  case MADV_SEQUENTIAL: {
    MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_LOW};
    windows::for_committed_batched(addr, rounded_size,
                                      VmPagePriorityInformation, &info,
                                      sizeof(info));
    return 0;
  }

  case MADV_RANDOM: {
    MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_BELOW_NORMAL};
    windows::for_committed_batched(addr, rounded_size,
                                      VmPagePriorityInformation, &info,
                                      sizeof(info));
    return 0;
  }

  case MADV_NORMAL: {
    MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_NORMAL};
    windows::for_committed_batched(addr, rounded_size,
                                      VmPagePriorityInformation, &info,
                                      sizeof(info));
    return 0;
  }

  //==========================================================================
  // MADV_COLD: Deprioritize pages for eviction (Linux 5.4+)
  //==========================================================================
  // Data-preserving priority demotion. Skip the syscall if pages are
  // already cold (max_priority <= 1) -- saves a syscall on repeated
  // MADV_COLD calls to the same range (common in allocator decay loops).
  case MADV_COLD: {
    ULONG max_prio = windows::wsex_max_priority(addr,
                                                 rounded_size);
    if (MEMORY_PRIORITY_LOWEST < max_prio) {
      MEMORY_PAGE_PRIORITY_INFORMATION info = {MEMORY_PRIORITY_LOWEST};
      windows::for_committed_batched(addr, rounded_size,
                                        VmPagePriorityInformation, &info,
                                        sizeof(info));
    }
    return 0;
  }

  //==========================================================================
  // MADV_PAGEOUT: Actively evict pages (Linux 5.4+)
  //==========================================================================
  // Single MBI walk with WSEX-driven conditional priority demotion.
  // MEM_RESET only on shared/read-only file pages (safe -- content in file).
  // Priority demotion conditional on WSEX max_priority > 1. WS evict always.
  case MADV_PAGEOUT: {
    auto ws = windows::byte_scratch(4096);
    if (!ws) return -ENOMEM;
    windows::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State != MEM_COMMIT)
        continue;

      // Shared/read-only file pages: mark discardable (content in file).
      if ((walk.entry->Type == MEM_MAPPED || walk.entry->Type == MEM_IMAGE) &&
          walk.entry->AllocationProtect != PAGE_WRITECOPY &&
          walk.entry->AllocationProtect != PAGE_EXECUTE_WRITECOPY)
        windows::vm_reset(walk.chunk, walk.chunk_size);

      MEMORY_RANGE_ENTRY entry;
      entry.VirtualAddress = walk.chunk;
      entry.NumberOfBytes = walk.chunk_size;

      ULONG max_prio = windows::wsex_max_priority(walk.chunk, walk.chunk_size);
      if (max_prio > 1) {
        MEMORY_PAGE_PRIORITY_INFORMATION prio = {MEMORY_PRIORITY_LOWEST};
        ::NtSetInformationVirtualMemory(current_process,
                                        VmPagePriorityInformation, 1,
                                        &entry, &prio, sizeof(prio));
      }

      // Flags=1: ~27% faster than Flags=0 with identical behavior (RA16.4).
      MEMORY_REMOVE_WORKING_SET_INFORMATION rm = {1};
      ::NtSetInformationVirtualMemory(current_process,
                                      VmRemoveFromWorkingSetInformation, 1,
                                      &entry, &rm, sizeof(rm));
    }
    return 0;
  }

  //==========================================================================
  // MADV_POPULATE_READ: Fault in pages for reading (Linux 5.14+)
  //==========================================================================
  case MADV_POPULATE_READ: {
    windows::prefetch_committed(addr, rounded_size);
    return 0;
  }

  //==========================================================================
  // MADV_POPULATE_WRITE: Fault in pages for writing (Linux 5.14+)
  //==========================================================================
  // PrefetchVirtualMemory handles bulk read-prefetch. For MAP_PRIVATE file
  // views (PAGE_WRITECOPY), we additionally trigger a per-page write to
  // materialize COW copies. This is the same operation Linux performs
  // internally (faultin_page with FAULT_FLAG_WRITE).
  case MADV_POPULATE_WRITE: {
    windows::prefetch_committed(addr, rounded_size);

    auto ws = windows::byte_scratch(4096);
    if (!ws) return -ENOMEM;
    windows::RegionWalker walk(addr, rounded_size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State != MEM_COMMIT)
        continue;

      DWORD base_prot = walk.entry->Protect & 0xFF;

      if (base_prot == PAGE_READONLY || base_prot == PAGE_EXECUTE_READ ||
          base_prot == PAGE_NOACCESS || base_prot == PAGE_EXECUTE)
        return -EFAULT;

      if (base_prot == PAGE_WRITECOPY ||
          base_prot == PAGE_EXECUTE_WRITECOPY) {
        const SIZE_T page_size = windows::get_page_size();
        windows::FaultGuard guard;
        if (windows::fault_guard_enter(&guard, windows::FAULT_GUARD_MEMORY)) {
          // AV or in-page error during CoW trigger.
          return -EIO;
        }
        for (char *page = walk.chunk;
             page < walk.chunk + walk.chunk_size; page += page_size) {
          volatile char *p = reinterpret_cast<volatile char *>(page);
          char c = *p;
          *p = c;
        }
        windows::fault_guard_leave(&guard);
      }
    }
    return 0;
  }

  //==========================================================================
  // Hints with no Windows equivalent -- succeed silently (advisory)
  //==========================================================================
  // Transparent huge pages and KSM are Linux-specific kernel features.
  // These constants are defined in the Windows sys-mman-macros header so
  // applications using them compile and get the expected no-op behavior.
  //==========================================================================
  // MADV_DONTDUMP / MADV_DODUMP: WER crash dump exclusion
  //==========================================================================
  // Directly manipulates the PEB WER gather list. WerFault.exe reads this
  // cross-process at crash time. No NT syscall -- pure user-mode PEB write.
  // ASan and jemalloc use DONTDUMP on shadow/arena memory to keep dumps small.
  case MADV_DONTDUMP:
    return wer_exclude(addr, rounded_size);

  case MADV_DODUMP:
    return wer_include(addr);

  case MADV_HUGEPAGE:
  case MADV_NOHUGEPAGE:
  case MADV_MERGEABLE:
  case MADV_UNMERGEABLE:
    return 0;

  default:
    return -EINVAL;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
