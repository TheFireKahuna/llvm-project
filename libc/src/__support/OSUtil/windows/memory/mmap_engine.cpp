//===---------- Windows mmap engine (kernel function) ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX mmap on Windows via NT kernel primitives.
//
// Two allocation models based on the mapping type:
//
//   One-shot (MEM_PRIVATE): Standard MAP_ANONYMOUS|MAP_PRIVATE uses
//     NtAllocateVirtualMemoryEx(MEM_RESERVE|MEM_COMMIT|MEM_WRITE_WATCH) —
//     single atomic syscall, 40% faster than placeholders. MEM_WRITE_WATCH
//     enables hardware dirty page tracking at zero extra cost (1 bit/page
//     bitmap), enabling CoW optimization for future fork() and efficient
//     dirty detection during split/remap. Supports interior release
//     for partial munmap and decommit+recommit for MAP_FIXED overwrite.
//
//   Placeholder (MEM_MAPPED): File mappings, MAP_SHARED, MAP_NORESERVE,
//     PROT_NONE, and large pages use the placeholder → section → view model:
//       1. Reserve placeholder (MEM_RESERVE_PLACEHOLDER) at target address
//       2. Create section (NtCreateSectionEx) or reserve-replace
//       3. Map section into placeholder (NtMapViewOfSectionEx)
//
// The MBI Type field (MEM_PRIVATE vs MEM_MAPPED) dispatches munmap, mprotect,
// and mremap without additional metadata.
//
// Section commit policy (placeholder model only):
//   SEC_RESERVE — pages committed on demand via VEH fault handler (default).
//   SEC_COMMIT  — pages committed immediately (MAP_POPULATE only)
//
// PROT_NONE bypasses section creation entirely (bare placeholder).
// Large pages use SEC_LARGE_PAGES with MEM_RESERVE|MEM_COMMIT.
//
// NT APIs throughout: NtAllocateVirtualMemoryEx, NtCreateSectionEx,
// NtMapViewOfSectionEx, NtFreeVirtualMemory.
//
//===----------------------------------------------------------------------===//

#include "mmap_engine.h"

#include "src/__support/CPP/bit.h"
#include "src/__support/CPP/utility.h"
#include "include/llvm-libc-macros/fcntl-macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/memory/fixed_range_guard.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/remap_transaction.h"
#include "src/__support/OSUtil/windows/memory/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/memory/memory_region.h"
#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/region_snapshot.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

// Serializes MAP_FIXED/MREMAP_FIXED (exclusive) against munmap (shared).
// See mmap_lock.h for the Linux mmap_lock analogy.
auto &g_mmap_lock = windows::g_mmap_lock;

using PlaceholderRange = windows::PlaceholderRange;
using windows::ViewSpec;

// Forward declarations — these helpers are defined later in this file but
// called from the alloc_fixed / alloc_hint / alloc_file paths above them.
// All return long: address-as-long on success, -errno on failure.
intptr_t map_file_into_placeholder(PlaceholderRange &ph,
                                            HANDLE file_handle,
                                            LARGE_INTEGER section_offset,
                                            int prot, int flags,
                                            int open_flags, int fd = -1,
                                            HANDLE *out_section = nullptr,
                                            DWORD *out_view_prot = nullptr,
                                            ULONG sec_flags = SEC_COMMIT);
intptr_t map_large_anon_into_placeholder(PlaceholderRange &ph,
                                                   DWORD prot,
                                                   HANDLE *out_section = nullptr,
                                                   DWORD sec_flags = SEC_LARGE_PAGES);
intptr_t map_anon_private_into_placeholder(PlaceholderRange &ph,
                                                       int prot);
intptr_t map_anon_reserve_into_placeholder(PlaceholderRange &ph,
                                                       int prot);
intptr_t map_file_private_into_placeholder(PlaceholderRange &ph,
                                                    HANDLE file_handle,
                                                    LARGE_INTEGER file_offset,
                                                    int prot);
intptr_t alloc_fixed_free(void *addr, SIZE_T size, int prot, int flags);
intptr_t promote_private_for_section(void *addr, SIZE_T size,
                                     HANDLE file_handle,
                                     LARGE_INTEGER section_offset,
                                     int prot, int open_flags, int fd);
ULONG section_access_from_fd(int open_flags);
DWORD section_page_prot_from_fd(int open_flags);


/// MAP_FIXED for MEM_PRIVATE anonymous memory (sub-range or full).
///
/// Simple decommit + recommit: zero-fills the target range in two syscalls.
/// No placeholder transitions, no VEH remap guard, no CAS retries.
///
/// The kernel's VAD lock serializes both calls against concurrent access.
/// The decommitted window (between decommit and recommit) is MEM_RESERVE
/// (inaccessible) — any concurrent access faults to SIGSEGV, which is
/// correct for POSIX MAP_FIXED semantics (replacing existing mappings).
///
/// Works uniformly on both one-shot allocations and placeholder-committed
/// MEM_PRIVATE regions. Surrounding committed data outside the target
/// range survives.
///
/// PROT_NONE: decommit only (MEM_RESERVE = inaccessible). Subsequent
/// mprotect to accessible re-commits with zero-fill.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_fixed_private_placeholder(void *addr, SIZE_T size,
                                                  int prot) {
  // Decommit: frees physical memory, pages become MEM_RESERVE (inaccessible).
  if (!windows::vm_decommit(addr, size))
    return -EINVAL;

  // PROT_NONE: decommitted pages are already inaccessible. Done.
  if (prot == PROT_NONE)
    return cpp::bit_cast<intptr_t>(addr);

  // Recommit: zero-filled pages with the requested protection.
  DWORD page_prot = windows::prot_to_page_flags(prot);
  NTSTATUS st = windows::vm_commit(addr, size, page_prot);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);

  return cpp::bit_cast<intptr_t>(addr);
}

/// MAP_FIXED across multiple AllocationBases.
///
/// Two-phase approach: validate+teardown, then allocate.
///
/// Phase 0 validates the entire range (reject MEM_IMAGE/MEM_MAPPED).
///
/// Phase 1 releases every MEM_PRIVATE region in [addr, addr+size) to
/// MEM_FREE via interior_release (partial) or vm_release (full allocation).
/// MEM_FREE gaps are left untouched. No placeholder gap-fill or coalesce.
///
/// Phase 2 allocates into the now-contiguous MEM_FREE range using the
/// standard alloc_fixed_free path (oneshot for 64KB-aligned standard case,
/// placeholder+commit otherwise).
///
/// Drops from ~6-12 syscalls (gap-fill + preserve × N + coalesce + commit)
/// to ~(N+1) where N is the number of overlapping allocations.
intptr_t alloc_fixed_private_multi(void *addr, SIZE_T size, int prot) {
  char *start = static_cast<char *>(addr);
  char *end = start + size;

  // Phase 0: Validate the entire range before any mutation. Reject if any
  // region is MEM_IMAGE or MEM_MAPPED — the caller must re-dispatch.
  char *cur = start;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!windows::query_region(cur, mbi))
      return -EINVAL;

    if (mbi.Type == MEM_IMAGE)
      return -EINVAL;

    if (mbi.Type == MEM_MAPPED)
      return -ENOMEM; // Caller dispatches to alloc_fixed_split_remap.

    char *region_end =
        static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    cur = (region_end < end) ? region_end : end;
  }

  // Phase 1 under lock: release every MEM_PRIVATE region to MEM_FREE.
  g_mmap_lock.acquire_exclusive();

  cur = start;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!windows::query_region(cur, mbi)) {
      g_mmap_lock.release_exclusive();
      return -EINVAL;
    }

    if (mbi.State == MEM_FREE) {
      char *region_end =
          static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      cur = (region_end < end) ? region_end : end;
      continue;
    }

    // Re-validate type inside the lock: if a concurrent operation changed
    // a MEM_PRIVATE region to MEM_MAPPED or MEM_IMAGE between Phase 0 and
    // Phase 1, bail out so the caller's retry loop can re-classify.
    if (mbi.Type == MEM_MAPPED || mbi.Type == MEM_IMAGE) {
      g_mmap_lock.release_exclusive();
      return -ENOMEM;
    }

    // MEM_PRIVATE: determine this allocation's full extent and compute
    // the overlap with the target range [start, end).
    char *ae = windows::find_alloc_end(mbi.AllocationBase);
    char *overlap_end = (ae < end) ? ae : end;
    SIZE_T overlap_size = static_cast<SIZE_T>(overlap_end - cur);

    // Release overlapping VA to MEM_FREE.
    if (cur == static_cast<char *>(mbi.AllocationBase) &&
        overlap_end >= ae) {
      // Full allocation: standard release (size=0).
      if (!windows::vm_release(mbi.AllocationBase)) {
        g_mmap_lock.release_exclusive();
        return -EINVAL;
      }
    } else {
      // Partial: interior_release on the sub-range.
      if (!windows::interior_release(cur, overlap_size)) {
        g_mmap_lock.release_exclusive();
        return -EINVAL;
      }
    }

    cur = overlap_end;
  }

  g_mmap_lock.release_exclusive();

  // Phase 2: entire [start, end) is now MEM_FREE. Allocate using the
  // standard MAP_FIXED-into-free path (handles 64KB alignment, PROT_NONE,
  // and placeholder fallback for non-64KB-aligned addresses).
  return alloc_fixed_free(addr, size, prot, 0);
}

/// MAP_FIXED within an existing section view via transactional split-remap.
///
/// Uses RemapTransaction to atomically carve the target range out of the
/// containing view, remap the kept left/right fragments, and map the new
/// allocation into the freed placeholder. On any failure, the transaction
/// destructor rolls back: re-remaps the original view, restores COW pages,
/// and transitions the mapping table entry back to LIVE.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_fixed_split_remap(void *addr, SIZE_T size, int prot,
                                          int flags) {
  // Find the containing view's base and end.
  char *view_base;
  char *view_end;
  if (!windows::find_alloc_range(addr, view_base, view_end))
    return -EINVAL;

  uintptr_t vb = reinterpret_cast<uintptr_t>(view_base);
  uintptr_t ve = reinterpret_cast<uintptr_t>(view_end);
  uintptr_t target_start = reinterpret_cast<uintptr_t>(addr);
  uintptr_t target_end = target_start + size;

  // Clamp to view boundaries.
  if (target_start < vb)
    target_start = vb;
  if (target_end > ve)
    target_end = ve;

  windows::RemapTransaction txn(view_base, view_end, target_start,
                                target_end - target_start);

  if (!txn.prepare()) {
    // No section handle or snapshot failed.
    return -EINVAL;
  }

  if (!txn.entry().spec.section) {
    // No section handle — can't split-remap (e.g., large pages).
    // ~txn will call abort_remap().
    return -EINVAL;
  }

  auto result = txn.execute();
  if (!result.target)
    return -ENOMEM; // ~txn rolls back.

  // Map new allocation into target placeholder.
  intptr_t map_result;
  if (prot == PROT_NONE) {
    map_result = cpp::bit_cast<intptr_t>(result.target.consume().base);
  } else if (flags & MAP_NORESERVE) {
    map_result = map_anon_reserve_into_placeholder(result.target, prot);
  } else {
    map_result = map_anon_private_into_placeholder(result.target, prot);
  }

  if (LIBC_UNLIKELY(map_result < 0))
    return map_result; // ~result.target releases placeholder.

  txn.commit();
  return map_result;
}

/// Freeze bracket: suspend all other threads via per-thread
/// NtCreateThreadStateChange, execute the callback, then resume
/// by closing each state-change handle.
///
/// Uses NtGetNextThread to enumerate threads without a snapshot, skipping
/// the calling thread (compared via NtCurrentThreadId from TEB). Each
/// target thread gets its own NtCreateThreadStateChange handle — crash-safe:
/// closing handles auto-resumes the targets. No thread is permanently stuck
/// if the mmap code crashes between suspend and resume.
///
/// NtCreateProcessStateChange + ProcessStateSuspend on NtCurrentProcess()
/// would DEADLOCK — it freezes the calling thread too. Per-thread suspension
/// is the only correct in-process freeze mechanism (confirmed RA14/Frontier 3).
///
/// Cost: ~898ns/thread with pre-allocated handles, ~50.8μs for 4 threads
/// full cycle (RA16.1). Only fires on Tier 2 (Tier 1 hint path failure).
///
/// Returns true if the callback succeeded. On failure (enumeration or
/// freeze unavailable), returns false without calling the callback.
template <typename Fn>
bool freeze_bracket(Fn &&callback) {
  constexpr int MAX_THREADS = 256;
  HANDLE sc_handles[MAX_THREADS];
  HANDLE thread_handles[MAX_THREADS];
  int frozen_count = 0;

  DWORD self_tid = ::NtCurrentThreadId();
  HANDLE process = NtCurrentProcess();

  // Enumerate all threads via NtGetNextThread. Each call returns a new
  // handle to the "next" thread after `prev`. We must dup the handle
  // before closing `prev` for threads we want to keep, because
  // NtGetNextThread needs the previous handle to find the successor.
  //
  // Strategy: NtGetNextThread returns a fresh handle each call. We close
  // the previous iteration's handle at the top of the loop. For threads
  // we freeze, we store the handle and must NOT close it as `prev` in
  // the next iteration — so we dup it for continued enumeration.
  HANDLE prev = nullptr;
  HANDLE next = nullptr;
  while (NT_SUCCESS(::NtGetNextThread(process, prev, THREAD_ALL_ACCESS,
                                      0, 0, &next))) {
    // Close the previous iteration handle (safe — we dup'd it if frozen).
    if (prev)
      ::NtClose(prev);
    prev = next;

    // Skip self — compare TID via ThreadBasicInformation.
    THREAD_BASIC_INFORMATION tbi;
    NTSTATUS qst = ::NtQueryInformationThread(
        next, ThreadBasicInformation, &tbi, sizeof(tbi), nullptr);
    if (NT_SUCCESS(qst) &&
        reinterpret_cast<uintptr_t>(tbi.ClientId.UniqueThread) ==
            static_cast<uintptr_t>(self_tid))
      continue;

    if (frozen_count >= MAX_THREADS)
      break;

    // Create state-change handle and suspend.
    HANDLE sc = nullptr;
    auto sc_oa = windows::internal_oa();
    NTSTATUS st = ::NtCreateThreadStateChange(
        &sc, THREAD_STATE_ALL_ACCESS, &sc_oa, next, 0);
    if (NT_ERROR(st))
      continue; // Thread may have exited — skip.

    st = ::NtChangeThreadState(sc, next, ThreadStateSuspend,
                               nullptr, 0, 0);
    if (NT_ERROR(st)) {
      ::NtClose(sc);
      continue;
    }

    // Dup the thread handle for the frozen array. We need `prev` (== next)
    // to stay valid for enumeration, but also need a handle for resume.
    HANDLE dup_thread = nullptr;
    ::NtDuplicateObject(process, next, process, &dup_thread,
                        0, 0, DUPLICATE_SAME_ACCESS);
    sc_handles[frozen_count] = sc;
    thread_handles[frozen_count] = dup_thread;
    ++frozen_count;
  }
  // Close the last enumeration handle.
  if (prev)
    ::NtClose(prev);

  // Execute the callback with all other threads frozen.
  bool ok = callback();

  // Resume all frozen threads by closing state-change handles.
  // Closing the state-change handle auto-resumes the target thread.
  for (int i = 0; i < frozen_count; ++i) {
    ::NtClose(sc_handles[i]);
    ::NtClose(thread_handles[i]);
  }
  return ok;
}

/// Promote MEM_PRIVATE VA to a section view for MAP_FIXED file/shared.
///
/// When MAP_FIXED with a file mapping targets existing one-shot (MEM_PRIVATE)
/// VA, this function provides a fast 2-syscall path:
///
///   Tier 1: interior_release(target) → section_hint_map(section, target)
///           ~1515 ns, 0% failure under mmap_lock serialization.
///
///   Tier 2: Freeze bracket (NtCreateProcessStateChange suspends all threads)
///           → re-teardown → section_hint_map → resume.
///           4 syscalls. Handles the rare case where concurrent
///           non-mmap VA operations (DLL loads, heap) claim the VA between
///           interior_release and section_hint_map.
///
/// Falls back to the caller (which uses the full FixedRangeGuard +
/// placeholder path) if both tiers fail.
///
/// Only handles MAP_SHARED file mappings (section views). MAP_PRIVATE
/// file mappings use demand-read via VEH and require the placeholder model.
///
/// The caller must verify the target range is entirely MEM_PRIVATE before
/// calling. Mixed regions (MEM_PRIVATE + MEM_FREE/MEM_MAPPED) should use
/// the standard alloc_file_fixed path.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t promote_private_for_section(void *addr, SIZE_T size,
                                     HANDLE file_handle,
                                     LARGE_INTEGER section_offset,
                                     int prot, int open_flags, int fd) {
  HANDLE process = NtCurrentProcess();

  // --- Create section ---
  // Reuse cached section from the fd entry when available.
  HANDLE section = nullptr;
  bool section_owned = true;
  if (fd >= 0) {
    internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
    if (ofd)
      section = ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE);
    if (section)
      section_owned = false;
  }

  if (!section) {
    DWORD section_prot = section_page_prot_from_fd(open_flags);
    ULONG section_access = section_access_from_fd(open_flags) | SECTION_QUERY;

    // SEC_64K_PAGES for shared file sections ≥64KB (promote path is
    // always MAP_SHARED — MAP_PRIVATE uses the placeholder model).
    ULONG sec_flags = SEC_COMMIT;
    if (size >= 65536)
      sec_flags |= SEC_64K_PAGES;

    auto sec_oa = windows::internal_oa();
    NTSTATUS st = ::NtCreateSectionEx(&section, section_access, &sec_oa,
                                       nullptr, section_prot, sec_flags,
                                       file_handle, nullptr, 0);
    if (NT_ERROR(st) && sec_flags != SEC_COMMIT) {
      // Fall back to plain SEC_COMMIT if SEC_64K_PAGES fails.
      st = ::NtCreateSectionEx(&section, section_access, &sec_oa,
                                nullptr, section_prot, SEC_COMMIT,
                                file_handle, nullptr, 0);
    }
    if (NT_ERROR(st))
      return windows_util::ntstatus_to_kerr(st);

    // Cache in the fd entry.
    if (fd >= 0) {
      internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
      if (ofd && !ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE)) {
        HANDLE dup = nullptr;
        HANDLE expected = nullptr;
        if (NT_SUCCESS(::NtDuplicateObject(process, section, process, &dup,
                                           0, 0, DUPLICATE_SAME_ACCESS)) &&
            !ofd->disk().section_handle.compare_exchange_strong(
                expected, dup, cpp::MemoryOrder::RELEASE,
                cpp::MemoryOrder::RELAXED))
          ::NtClose(dup);
      }
    }
  }

  DWORD view_prot = windows::prot_to_page_flags(prot);

  // --- Tier 1: Release + hint map under mmap_lock ---
  //
  // The mmap_lock serializes MAP_FIXED against concurrent munmap. No
  // other mmap operation can claim this VA while we hold the lock.
  // Non-mmap VA operations (DLL loads, heap) are not serialized, but
  // have ~0% collision probability with specific addresses under the lock.
  g_mmap_lock.acquire_exclusive();

  // Tear down all MEM_PRIVATE in the target range to MEM_FREE.
  char *start = static_cast<char *>(addr);
  char *end = start + size;
  char *cur = start;
  bool teardown_ok = true;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!windows::query_region(cur, mbi)) {
      teardown_ok = false;
      break;
    }

    if (mbi.State == MEM_FREE) {
      char *region_end =
          static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      cur = (region_end < end) ? region_end : end;
      continue;
    }

    if (mbi.Type != MEM_PRIVATE) {
      teardown_ok = false;
      break;
    }

    char *ae = windows::find_alloc_end(mbi.AllocationBase);
    char *overlap_end = (ae < end) ? ae : end;
    SIZE_T overlap_size = static_cast<SIZE_T>(overlap_end - cur);

    if (cur == static_cast<char *>(mbi.AllocationBase) && overlap_end >= ae) {
      if (!windows::vm_release(mbi.AllocationBase)) {
        teardown_ok = false;
        break;
      }
    } else {
      if (!windows::interior_release(cur, overlap_size)) {
        teardown_ok = false;
        break;
      }
    }
    cur = overlap_end;
  }

  if (!teardown_ok) {
    g_mmap_lock.release_exclusive();
    if (section_owned)
      ::NtClose(section);
    return -EINVAL;
  }

  // Attempt section_hint_map directly into the freed VA.
  NTSTATUS map_st = windows::section_hint_map(
      section, addr, size, section_offset, view_prot);

  g_mmap_lock.release_exclusive();

  bool mapped = NT_SUCCESS(map_st);

  // --- Tier 2: Freeze bracket ---
  //
  // If Tier 1 failed (STATUS_CONFLICTING_ADDRESSES), another allocation
  // (DLL load, heap) claimed the VA between interior_release and
  // section_hint_map. Freeze all other threads and retry.
  if (!mapped) {
    mapped = freeze_bracket([&]() -> bool {
      // Re-check and re-teardown if needed.
      char *c = start;
      while (c < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!windows::query_region(c, mbi))
          return false;

        if (mbi.State == MEM_FREE) {
          char *re = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
          c = (re < end) ? re : end;
          continue;
        }

        // Something claimed the VA. Only MEM_PRIVATE can be torn down.
        if (mbi.Type != MEM_PRIVATE)
          return false;

        char *ae = windows::find_alloc_end(mbi.AllocationBase);
        char *oe = (ae < end) ? ae : end;
        SIZE_T os = static_cast<SIZE_T>(oe - c);

        if (c == static_cast<char *>(mbi.AllocationBase) && oe >= ae) {
          if (!windows::vm_release(mbi.AllocationBase))
            return false;
        } else {
          if (!windows::interior_release(c, os))
            return false;
        }
        c = oe;
      }

      NTSTATUS st = windows::section_hint_map(
          section, addr, size, section_offset, view_prot);
      return NT_SUCCESS(st);
    });
  }

  if (!mapped) {
    // Both tiers failed. Clean up section and let the caller fall back
    // to the full FixedRangeGuard + placeholder path.
    if (section_owned)
      ::NtClose(section);
    return -EAGAIN;
  }

  // --- Register the mapping ---
  HANDLE dup_file = nullptr;
  if (file_handle) {
    ::NtDuplicateObject(process, file_handle, process, &dup_file,
                        0, 0, DUPLICATE_SAME_ACCESS);
  }
  HANDLE reg_section = section;
  if (!section_owned) {
    // Dup the section for the mapping table (fd entry owns the original).
    HANDLE dup_section = nullptr;
    if (NT_SUCCESS(::NtDuplicateObject(process, section, process,
                                       &dup_section, 0, 0,
                                       DUPLICATE_SAME_ACCESS)))
      reg_section = dup_section;
    else
      reg_section = nullptr;
  }
  windows::g_mapping_table.register_mapping_take(
      addr, size,
      ViewSpec{reg_section, dup_file, section_offset, view_prot,
               /*flags=*/0});
  return cpp::bit_cast<intptr_t>(addr);
}

/// Decode MAP_HUGE_* size encoding from mmap flags.
/// Returns SEC_LARGE_PAGES (2MB) or SEC_HUGE_PAGES (1GB).
DWORD decode_huge_page_sec_flags(int flags) {
  int shift = (flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK;
  if (shift == 30) // MAP_HUGE_1GB = (30 << 26)
    return SEC_HUGE_PAGES;
  return SEC_LARGE_PAGES; // Default: 2MB (shift==21 or unspecified)
}

/// Get the page alignment for a given section flag.
/// SEC_LARGE_PAGES: GetLargePageMinimum() (typically 2MB).
/// SEC_HUGE_PAGES: 1GB.
SIZE_T huge_page_alignment(DWORD sec_flags) {
  if (sec_flags == SEC_HUGE_PAGES)
    return static_cast<SIZE_T>(1) << 30; // 1GB
  return windows::get_large_page_minimum();
}

/// Anonymous large/huge page allocation (MAP_HUGETLB | MAP_ANONYMOUS).
///
/// Section-backed: NtCreateSectionEx(SEC_LARGE_PAGES or SEC_HUGE_PAGES) +
/// NtMapViewOfSectionEx, same placeholder model as regular paths.
/// Produces MEM_MAPPED views for uniform munmap dispatch.
///
/// MAP_HUGE_2MB → SEC_LARGE_PAGES (2MB, default).
/// MAP_HUGE_1GB → SEC_HUGE_PAGES (1GB).
///
/// Requires SeLockMemoryPrivilege. Size/alignment determined by page type.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_large_pages(void *hint, SIZE_T size, int prot,
                                   int flags) {
  DWORD sec_flags = decode_huge_page_sec_flags(flags);
  SIZE_T page_align = huge_page_alignment(sec_flags);
  if (page_align == 0)
    return -ENOTSUP;

  SIZE_T rounded = windows::round_up_to_align(size, page_align);
  if (LIBC_UNLIKELY(rounded == 0))
    return -ENOMEM;
  DWORD page_prot = windows::prot_to_page_flags(prot);

  if ((flags & MAP_FIXED) && hint) {
    windows::FixedRangeGuard guard;
    if (int err = guard.prepare(hint, rounded))
      return -static_cast<intptr_t>(err);

    // hint is always 64KB-aligned for large pages → placeholder ready.
    PlaceholderRange ph = PlaceholderRange::from_raw(hint, rounded);
    guard.release_lock();

    HANDLE section = nullptr;
    intptr_t result =
        map_large_anon_into_placeholder(ph, page_prot, &section, sec_flags);

    if (LIBC_UNLIKELY(result < 0))
      return result; // ~guard aborts remap guard, ~ph releases placeholder.

    // Commit: sentinel REMAPPING -> LIVE with the new mapping data.
    windows::g_mapping_table.commit_remap(hint, reinterpret_cast<void *>(result),
                                           rounded,
                                           ViewSpec{section, /*file=*/nullptr,
                                                    /*offset=*/{}, page_prot,
                                                    /*flags=*/0});
    guard.mark_committed();
    if (section)
      ::NtClose(section);
    return result;
  }

  // Non-fixed: placeholder then section + map.
  void *base = (hint && windows::is_alloc_aligned(hint)) ? hint : nullptr;
  PlaceholderRange ph = PlaceholderRange::reserve(rounded, base);
  if (!ph && base)
    ph = PlaceholderRange::reserve(rounded);

  if (LIBC_UNLIKELY(!ph))
    return -ENOMEM;

  HANDLE section = nullptr;
  intptr_t result =
      map_large_anon_into_placeholder(ph, page_prot, &section, sec_flags);
  if (LIBC_UNLIKELY(result < 0))
    return result; // ~ph releases the placeholder.

  windows::g_mapping_table.register_mapping_take(
      reinterpret_cast<void *>(result), rounded,
      ViewSpec{section, /*file=*/nullptr, /*offset=*/{}, page_prot,
               /*flags=*/0});
  return result;
}

/// File-backed large/huge page allocation (MAP_HUGETLB without MAP_ANONYMOUS).
///
/// Uses the same placeholder model as regular file mappings:
///   create_placeholder → map_file_into_placeholder(SEC_LARGE_PAGES/SEC_HUGE_PAGES)
///
/// MAP_HUGE_2MB → SEC_LARGE_PAGES (2MB, default).
/// MAP_HUGE_1GB → SEC_HUGE_PAGES (1GB).
///
/// The section handle is kept alive and registered in the mapping table,
/// enabling consistent munmap and NUMA remap.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_file_large_pages(void *hint, SIZE_T size, int prot,
                                         int flags, HANDLE file_handle,
                                         off_t offset, int open_flags,
                                         int fd) {
  DWORD sec_flags = decode_huge_page_sec_flags(flags);
  SIZE_T page_align = huge_page_alignment(sec_flags);
  if (page_align == 0)
    return -ENOTSUP;

  SIZE_T rounded = windows::round_up_to_align(size, page_align);
  if (LIBC_UNLIKELY(rounded == 0))
    return -ENOMEM;
  LARGE_INTEGER section_offset;
  section_offset.QuadPart = static_cast<LONGLONG>(offset);

  if ((flags & MAP_FIXED) && hint) {
    if (LIBC_UNLIKELY(!windows::is_alloc_aligned(hint)))
      return -EINVAL;

    windows::FixedRangeGuard guard;
    if (int err = guard.prepare(hint, rounded))
      return -static_cast<intptr_t>(err);

    // hint is always 64KB-aligned (checked above) → placeholder ready.
    PlaceholderRange ph = PlaceholderRange::from_raw(hint, rounded);
    guard.release_lock();

    HANDLE out_section = nullptr;
    DWORD out_prot = 0;
    intptr_t view = map_file_into_placeholder(ph, file_handle,
                                           section_offset, prot, flags,
                                           open_flags, fd, &out_section,
                                           &out_prot, sec_flags);

    if (LIBC_UNLIKELY(view < 0))
      return view; // ~guard aborts, ~ph releases.

    windows::g_mapping_table.commit_remap(hint, reinterpret_cast<void *>(view),
                                           rounded,
                                           ViewSpec{out_section, file_handle,
                                                    section_offset, out_prot,
                                                    /*flags=*/0});
    guard.mark_committed();
    if (out_section)
      ::NtClose(out_section);
    return view;
  }

  // Non-fixed: create placeholder at hint or system-chosen address.
  void *base = (hint && windows::is_alloc_aligned(hint)) ? hint : nullptr;
  PlaceholderRange ph = PlaceholderRange::reserve(rounded, base);
  if (!ph && base)
    ph = PlaceholderRange::reserve(rounded);

  if (LIBC_UNLIKELY(!ph))
    return -ENOMEM;

  HANDLE out_section = nullptr;
  DWORD out_prot = 0;
  intptr_t view = map_file_into_placeholder(ph, file_handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot, sec_flags);
  if (LIBC_UNLIKELY(view < 0))
    return view; // ~ph releases the placeholder.

  // Dup the borrowed file_handle (fd_table retains its copy); transfer
  // the freshly-created out_section directly (no dup + close cycle).
  HANDLE dup_file = nullptr;
  if (file_handle) {
    NTSTATUS dup_st = ::NtDuplicateObject(
        NtCurrentProcess(), file_handle, NtCurrentProcess(), &dup_file, 0, 0,
        DUPLICATE_SAME_ACCESS);
    if (NT_ERROR(dup_st)) {
      ::NtClose(out_section);
      ::NtUnmapViewOfSectionEx(NtCurrentProcess(),
                               reinterpret_cast<void *>(view), 0);
      return -ENOMEM;
    }
  }
  windows::g_mapping_table.register_mapping_take(
      reinterpret_cast<void *>(view), rounded,
      ViewSpec{out_section, dup_file, section_offset, out_prot, /*flags=*/0});
  return view;
}

/// Derive section access rights from the fd's open_flags.
/// The section's access caps what any view can later mprotect to.
///
/// EXECUTE is deliberately excluded: internal::open() never grants
/// FILE_EXECUTE on the file handle, so NtCreateSectionEx with
/// PAGE_EXECUTE_* would fail with STATUS_ACCESS_DENIED. Omitting
/// SECTION_MAP_EXECUTE also hardens against accidental W^X violations —
/// mprotect(PROT_EXEC) on a file mapping will correctly fail.
ULONG section_access_from_fd(int open_flags) {
  int accmode = open_flags & O_ACCMODE;
  if (accmode == O_RDWR)
    return SECTION_MAP_READ | SECTION_MAP_WRITE;
  if (accmode == O_WRONLY)
    return SECTION_MAP_WRITE;
  return SECTION_MAP_READ;
}

/// Derive section page protection from the fd's open_flags.
/// Must be compatible with the file handle's access rights.
///
/// No EXECUTE: file handles from internal::open() lack FILE_EXECUTE,
/// so PAGE_EXECUTE_* would cause STATUS_ACCESS_DENIED. This is also
/// better security — section objects are created with the minimum
/// necessary protection, preventing later mprotect to executable.
DWORD section_page_prot_from_fd(int open_flags) {
  int accmode = open_flags & O_ACCMODE;
  if (accmode == O_RDWR)
    return PAGE_READWRITE;
  // Read-only (or write-only, which validate_file_prot rejects anyway).
  return PAGE_READONLY;
}

/// Validate that the requested prot/flags are compatible with the fd's
/// open_flags. Returns 0 if valid, errno value on failure.
int validate_file_prot(int prot, int flags, int open_flags) {
  int accmode = open_flags & O_ACCMODE;
  bool wants_write = (prot & PROT_WRITE) != 0;
  bool is_shared = (flags & MAP_SHARED) != 0;

  // MAP_SHARED + PROT_WRITE requires a writable fd.
  if (is_shared && wants_write && accmode == O_RDONLY)
    return EACCES;

  // MAP_PRIVATE + PROT_WRITE (COW) only needs read access.
  // All other combinations are fine with any access mode.

  // O_WRONLY fds can't create file mappings (Windows requires read access
  // on the file handle to create a section object).
  if (accmode == O_WRONLY)
    return EACCES;

  return 0;
}

/// Map a file view into a placeholder using NT APIs.
///
/// NtCreateSectionEx + NtMapViewOfSectionEx provide:
///   - NTSTATUS return for precise error discrimination
///   - In/out SectionOffset and ViewSize (kernel rounds offset to
///     allocation granularity and reports the actual mapped size)
///   - Direct section handle without Win32 GetLastError() dance
///
/// The section handle is kept open and returned via *out_section for
/// registration in the mapping table. This enables NUMA remap via
/// NtUnmapViewOfSectionEx + NtMapViewOfSectionEx without recreating the
/// section. The caller is responsible for closing it (typically via the
/// mapping table's remove()).
///
/// Returns address-as-long on success, -errno on failure.
intptr_t map_file_into_placeholder(PlaceholderRange &ph,
                                            HANDLE file_handle,
                                            LARGE_INTEGER section_offset,
                                            int prot, int flags,
                                            int open_flags, int fd,
                                            HANDLE *out_section,
                                            DWORD *out_view_prot,
                                            ULONG sec_flags) {
  HANDLE process = NtCurrentProcess();

  // Reuse cached section handle when available. memfd_create / shm_open
  // set section_handle before the fd is returned (write-once, no race).
  // Regular file fds fill it lazily on the first mmap via CAS (see below).
  // Multiple views of the same section share physical pages — required for
  // MAP_SHARED coherence on pagefile-backed anonymous shared memory and the
  // double-MAP_FIXED ring buffer pattern.
  HANDLE section = nullptr;
  bool section_owned = true; // true = we created it, must close on error.
  if (fd >= 0) {
    internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
    if (ofd)
      section = ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE);
    if (section)
      section_owned = false; // fd entry owns it — don't close.
  }

  if (!section) {
    DWORD section_prot = section_page_prot_from_fd(open_flags);
    ULONG section_access = section_access_from_fd(open_flags);

    // Include SECTION_QUERY for NtQuerySection diagnostics and
    // SECTION_EXTEND_SIZE for future ftruncate-driven growth.
    section_access |= SECTION_QUERY;

    // SEC_64K_PAGES: use 64K contiguous pages for MAP_SHARED mappings
    // ≥64KB. 34% read speedup from reduced TLB pressure (RA14/Frontier 4),
    // zero privilege requirement, placeholder-compatible (RA16.2).
    // Only for SEC_COMMIT (not SEC_RESERVE or SEC_LARGE_PAGES).
    bool is_shared = !(flags & MAP_PRIVATE);
    ULONG effective_sec_flags = sec_flags;
    if (is_shared && sec_flags == SEC_COMMIT && ph.size() >= 65536)
      effective_sec_flags |= SEC_64K_PAGES;

    auto sec_oa = windows::internal_oa();
    NTSTATUS status = ::NtCreateSectionEx(
        &section, section_access, &sec_oa, nullptr, section_prot,
        effective_sec_flags, file_handle, nullptr, 0);
    if (NT_ERROR(status)) {
      // SEC_64K_PAGES may fail on older builds or certain file types.
      // Fall back to plain SEC_COMMIT.
      if (effective_sec_flags != sec_flags) {
        status = ::NtCreateSectionEx(
            &section, section_access, &sec_oa, nullptr, section_prot,
            sec_flags, file_handle, nullptr, 0);
      }
    }
    if (NT_ERROR(status))
      return windows_util::ntstatus_to_kerr(status);

    // Cache in the fd entry so subsequent mmap calls skip NtCreateSectionEx.
    // Pre-check before NtDuplicateObject: NtCreateSectionEx serializes on
    // the kernel's SectionObjectPointers lock, so threads exit one at a time
    // and the winner likely fills the cache before losers even reach here.
    // The pre-check avoids the dup+close syscall pair for those losers.
    if (fd >= 0) {
      internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
      if (ofd) {
        HANDLE expected = nullptr;
        if (ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE) == nullptr) {
          HANDLE dup = nullptr;
          if (NT_SUCCESS(::NtDuplicateObject(process, section, process, &dup,
                                             0, 0, DUPLICATE_SAME_ACCESS)) &&
              !ofd->disk().section_handle.compare_exchange_strong(
                  expected, dup, cpp::MemoryOrder::RELEASE,
                  cpp::MemoryOrder::RELAXED))
            ::NtClose(dup); // Lost race — winner's handle already stored.
        }
      }
    }
  }

  // Determine the view's page protection.
  bool is_private = (flags & MAP_PRIVATE) != 0;
  DWORD view_prot = is_private ? windows::prot_to_page_flags_cow(prot)
                               : windows::prot_to_page_flags(prot);

  // NtMapViewOfSectionEx takes in/out BaseAddress, SectionOffset, ViewSize.
  // SectionOffset must be page-aligned. The kernel writes back actual values.
  PVOID base_address = ph.base();
  SIZE_T actual_view_size = ph.size();

  NTSTATUS map_st = ::NtMapViewOfSectionEx(section, process, &base_address,
                                            &section_offset, &actual_view_size,
                                            MEM_REPLACE_PLACEHOLDER, view_prot,
                                            nullptr, 0);

  if (NT_ERROR(map_st)) {
    if (section_owned)
      ::NtClose(section);
    return windows_util::ntstatus_to_kerr(map_st);
  }

  (void)ph.consume(); // Placeholder consumed by the section view.

  if (out_section)
    *out_section = section;
  else if (section_owned)
    ::NtClose(section);

  if (out_view_prot)
    *out_view_prot = view_prot;

  return cpp::bit_cast<intptr_t>(base_address);
}

/// Check if a VA range is entirely MEM_PRIVATE (committed or reserved).
/// Returns true if every byte in [addr, addr+size) belongs to a MEM_PRIVATE
/// allocation. MEM_FREE gaps return false.
///
/// Uses NtPssCaptureVaSpaceBulk via RegionWalker for bulk queries —
/// 1.3-3.2x faster than iterative NtQueryVirtualMemory (RA14/Frontier 2).
bool is_range_all_private(void *addr, SIZE_T size) {
  auto ws = windows::byte_scratch(4096);
  if (!ws) return false;
  windows::RegionWalker walk(addr, size,
      reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(ws.data()), ws.size());
  while (walk.next()) {
    if (walk.entry->State == MEM_FREE || walk.entry->Type != MEM_PRIVATE)
      return false;
  }
  return true;
}

/// File-backed MAP_FIXED allocation. Tears down existing mappings, creates
/// a placeholder, and maps the file view into it.
///
/// When offset is not page-aligned, the placeholder is enlarged to start
/// at the page-aligned-down offset, and the returned pointer is adjusted
/// forward by the delta (at most page_size - 1 bytes).
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_file_fixed(void *addr, SIZE_T size, int prot, int flags,
                                   HANDLE file_handle, off_t offset,
                                   int open_flags, int fd) {
  const DWORD64 aligned_offset =
      windows::round_down_to_page_offset(static_cast<DWORD64>(offset));
  const SIZE_T delta = static_cast<SIZE_T>(offset - aligned_offset);
  const SIZE_T view_size = windows::round_to_page(size + delta);
  if (LIBC_UNLIKELY(view_size == 0))
    return -ENOMEM;

  // Placeholder must start delta bytes before addr to accommodate the
  // kernel's page-aligned view base (delta is at most page_size - 1).
  char *placeholder_addr = static_cast<char *>(addr) - delta;

  if (LIBC_UNLIKELY(!windows::is_page_aligned(placeholder_addr)))
    return -EINVAL;

  // --- Promote fast path: MAP_SHARED into all-MEM_PRIVATE VA ---
  //
  // When the target range is entirely MEM_PRIVATE (one-shot allocations),
  // try the 2-syscall promotion path: interior_release → section_hint_map.
  // This avoids the FixedRangeGuard → placeholder → section-view pipeline.
  //
  // MAP_PRIVATE file mappings use demand-read via VEH and require the
  // placeholder model — only MAP_SHARED benefits from promotion.
  if (!(flags & MAP_PRIVATE) && is_range_all_private(placeholder_addr,
                                                      view_size)) {
    LARGE_INTEGER section_offset;
    section_offset.QuadPart = static_cast<LONGLONG>(aligned_offset);
    intptr_t result = promote_private_for_section(
        placeholder_addr, view_size, file_handle, section_offset, prot,
        open_flags, fd);
    if (result >= 0)
      return result + delta;
    // -EAGAIN: promotion failed, fall through to placeholder path.
    // Other errors: propagate (real failures like EBADF, EINVAL).
    if (result != -EAGAIN)
      return result;
  }

  windows::FixedRangeGuard guard;
  if (int err = guard.prepare(placeholder_addr, view_size))
    return -static_cast<intptr_t>(err);

  PlaceholderRange ph;
  if (windows::is_alloc_aligned(placeholder_addr)) {
    // 64KB-aligned → prepare_for_fixed produced a placeholder.
    ph = PlaceholderRange::from_raw(placeholder_addr, view_size);
  } else {
    // Non-64KB: prepare_for_fixed produced MEM_FREE. Oversized + split.
    uintptr_t target = reinterpret_cast<uintptr_t>(placeholder_addr);
    uintptr_t aligned = windows::align_down_to_granularity(target);
    SIZE_T prefix = target - aligned;
    // prefix is at most alloc_granularity-1; guard before adding to view_size.
    if (LIBC_UNLIKELY(view_size > SIZE_MAX - prefix))
      return -ENOMEM; // ~guard releases lock + aborts remap guard.
    SIZE_T total = windows::round_to_page(prefix + view_size);
    if (LIBC_UNLIKELY(total == 0))
      return -ENOMEM; // ~guard releases lock + aborts remap guard.

    PlaceholderRange oversized = PlaceholderRange::reserve(
        total, reinterpret_cast<void *>(aligned));
    if (oversized && reinterpret_cast<uintptr_t>(oversized.base()) == aligned) {
      PlaceholderRange halves_left, halves_right;
      if (oversized.split(prefix, &halves_left, &halves_right)) {
        // halves_left = prefix (released by destructor).
        // halves_right = target + possible suffix.
        SIZE_T suffix = total - prefix - view_size;
        if (suffix > 0) {
          PlaceholderRange parts_left, parts_right;
          if (halves_right.split(view_size, &parts_left, &parts_right)) {
            ph = cpp::move(parts_left);
            // parts_right (suffix) released by destructor.
          }
          // If split fails, halves_right released by destructor.
        } else {
          ph = cpp::move(halves_right);
        }
        // halves_left released by destructor.
      }
      // If split fails, oversized already consumed by split attempt —
      // but split() leaves self valid on failure, so ~oversized releases.
    }
    // If oversized was at wrong address or null, ~oversized releases.
  }

  guard.release_lock();

  if (LIBC_UNLIKELY(!ph))
    return -ENOMEM; // ~guard aborts remap guard.

  if (LIBC_UNLIKELY(ph.base() != placeholder_addr))
    return -EINVAL; // ~guard aborts, ~ph releases.

  LARGE_INTEGER section_offset;
  section_offset.QuadPart = static_cast<LONGLONG>(aligned_offset);

  // MAP_PRIVATE file: private memory + VEH demand-read.
  // No commit_remap — private file mappings are not section-tracked.
  // ~guard aborts the sentinel (REMAPPING → KEY_FREE) on all paths.
  if (flags & MAP_PRIVATE) {
    intptr_t result = map_file_private_into_placeholder(
        ph, file_handle, section_offset, prot);
    if (LIBC_UNLIKELY(result < 0))
      return result; // ~guard aborts, ~ph releases.
    return result + delta;
  }

  // MAP_SHARED file: section view.
  HANDLE out_section = nullptr;
  DWORD out_prot = 0;
  intptr_t view = map_file_into_placeholder(ph, file_handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot);
  if (LIBC_UNLIKELY(view < 0))
    return view; // ~guard aborts, ~ph releases.

  // Commit: sentinel REMAPPING -> LIVE with the new file mapping data.
  void *view_base = reinterpret_cast<void *>(view);
  windows::g_mapping_table.commit_remap(placeholder_addr, view_base,
                                         view_size,
                                         ViewSpec{out_section, file_handle,
                                                  section_offset, out_prot,
                                                  /*flags=*/0});
  guard.mark_committed();
  if (out_section)
    ::NtClose(out_section);

  return view + delta;
}

/// File-backed allocation with optional address hint (non-MAP_FIXED).
/// Falls back to system-chosen address if the hint is unavailable.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_file_hint(void *hint, SIZE_T size, int prot, int flags,
                                  HANDLE file_handle, off_t offset,
                                  int open_flags, int fd) {
  const DWORD64 aligned_offset =
      windows::round_down_to_page_offset(static_cast<DWORD64>(offset));
  const SIZE_T delta = static_cast<SIZE_T>(offset - aligned_offset);
  const SIZE_T view_size = windows::round_to_page(size + delta);
  if (LIBC_UNLIKELY(view_size == 0))
    return -ENOMEM;

  // Adjust hint backward for alignment delta.
  PlaceholderRange ph;
  if (hint) {
    char *adjusted = static_cast<char *>(hint) - delta;
    if (windows::is_alloc_aligned(adjusted)) {
      ph = PlaceholderRange::reserve(view_size, adjusted);
    } else if (windows::is_page_aligned(adjusted)) {
      // Oversized placeholder + split to honor non-64KB hint.
      uintptr_t target = reinterpret_cast<uintptr_t>(adjusted);
      uintptr_t aligned = windows::align_down_to_granularity(target);
      SIZE_T prefix = target - aligned;
      // Overflow means hint can't be satisfied; fall through to system-chosen.
      SIZE_T total = 0;
      if (LIBC_LIKELY(view_size <= SIZE_MAX - prefix))
        total = windows::round_to_page(prefix + view_size);
      if (LIBC_LIKELY(total != 0)) {
        PlaceholderRange oversized = PlaceholderRange::reserve(
            total, reinterpret_cast<void *>(aligned));
        if (oversized && reinterpret_cast<uintptr_t>(oversized.base()) == aligned) {
          PlaceholderRange halves_left, halves_right;
          if (oversized.split(prefix, &halves_left, &halves_right)) {
            ph = cpp::move(halves_right);
            // halves_left (prefix) released by destructor.
          }
          // If split fails, ~oversized releases.
        }
        // If oversized was at wrong address or null, ~oversized releases.
      }
    }
  }

  // Fall back to system-chosen address (or MAP_32BIT constrained).
  if (!ph) {
    if (flags & MAP_32BIT)
      ph = PlaceholderRange::reserve_32bit(view_size);
    else
      ph = PlaceholderRange::reserve(view_size);
  }

  if (LIBC_UNLIKELY(!ph))
    return -ENOMEM;

  LARGE_INTEGER section_offset;
  section_offset.QuadPart = static_cast<LONGLONG>(aligned_offset);

  // MAP_PRIVATE file: private memory + VEH demand-read. No section.
  if (flags & MAP_PRIVATE) {
    intptr_t result = map_file_private_into_placeholder(
        ph, file_handle, section_offset, prot);
    if (LIBC_UNLIKELY(result < 0))
      return result; // ~ph releases the placeholder.
    return result + delta;
  }

  // MAP_SHARED file: section view (shared semantics require section).
  HANDLE out_section = nullptr;
  DWORD out_prot = 0;
  intptr_t view = map_file_into_placeholder(ph, file_handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot);
  if (LIBC_UNLIKELY(view < 0))
    return view; // ~ph releases the placeholder.

  void *view_base = reinterpret_cast<void *>(view);
  HANDLE dup_file = nullptr;
  if (file_handle) {
    NTSTATUS dup_st = ::NtDuplicateObject(
        NtCurrentProcess(), file_handle, NtCurrentProcess(), &dup_file, 0, 0,
        DUPLICATE_SAME_ACCESS);
    if (NT_ERROR(dup_st)) {
      ::NtClose(out_section);
      ::NtUnmapViewOfSectionEx(NtCurrentProcess(),
                               reinterpret_cast<void *>(view), 0);
      return -ENOMEM;
    }
  }
  windows::g_mapping_table.register_mapping_take(
      view_base, view_size,
      ViewSpec{out_section, dup_file, section_offset, out_prot, /*flags=*/0});

  return view + delta;
}

/// Create a pagefile-backed section and map it into a placeholder.
///
/// Create a pagefile-backed section with large/huge pages and map into a
/// placeholder. Supports SEC_LARGE_PAGES (2MB) and SEC_HUGE_PAGES (1GB).
///
/// Returns address-as-long on success, -errno on failure.
intptr_t map_large_anon_into_placeholder(PlaceholderRange &ph,
                                                   DWORD prot,
                                                   HANDLE *out_section,
                                                   DWORD sec_flags) {
  HANDLE process = NtCurrentProcess();

  LARGE_INTEGER sec_size;
  sec_size.QuadPart = static_cast<LONGLONG>(ph.size());

  HANDLE section = nullptr;
  auto sec_oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateSectionEx(
      &section,
      SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE | SECTION_QUERY,
      &sec_oa, &sec_size, PAGE_EXECUTE_READWRITE, sec_flags, nullptr,
      nullptr, 0);
  if (NT_ERROR(st)) {
    return (st == STATUS_PRIVILEGE_NOT_HELD || st == STATUS_ACCESS_DENIED)
               ? -static_cast<intptr_t>(EPERM)
               : windows_util::ntstatus_to_kerr(st);
  }

  PVOID base = ph.base();
  SIZE_T view_size = ph.size();
  LARGE_INTEGER offset = {};
  st = ::NtMapViewOfSectionEx(section, process, &base, &offset, &view_size,
                               MEM_REPLACE_PLACEHOLDER, prot, nullptr, 0);
  if (NT_ERROR(st)) {
    ::NtClose(section);
    return (st == STATUS_ACCESS_DENIED)
               ? -static_cast<intptr_t>(EPERM)
               : windows_util::ntstatus_to_kerr(st);
  }

  (void)ph.consume(); // Placeholder consumed by the section view.

  if (out_section)
    *out_section = section;
  else
    ::NtClose(section);

  return cpp::bit_cast<intptr_t>(base);
}

/// Replace a placeholder with a bare MEM_RESERVE region. Single syscall,
/// no section, no mapping table, no commit charge. Pages are committed
/// on demand by the VEH fault handler.
///
/// \p prot is stored as AllocationProtect on the reserved region. The VEH
/// handler reads AllocationProtect from MBI to determine the commit
/// protection — same pattern as SEC_RESERVE section views.
///
/// Used for MAP_NORESERVE — eliminates the SEC_RESERVE section path
/// entirely. Partial munmap is trivial: MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER
/// works directly on reserved pages (no decommit step needed).
///
/// Returns address-as-long on success, -errno on failure.
intptr_t map_anon_reserve_into_placeholder(PlaceholderRange &ph,
                                                       int prot) {
  void *base = ph.base();
  NTSTATUS st = ph.reserve_replace(windows::prot_to_page_flags(prot));
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  return cpp::bit_cast<intptr_t>(base);
}

/// File MAP_PRIVATE via private memory + VEH demand-read. No section.
///
/// Reserve-replaces the placeholder, dups the file handle, and registers
/// a mapping table entry with VM_FLAG_FILE_PRIVATE. The VEH handler
/// commits pages on fault and reads file content via NtReadFile.
///
/// Partial munmap is trivial: table.remove() + MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER.
/// No split-remap, no COW protocol, no VEH remap guard.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t map_file_private_into_placeholder(PlaceholderRange &ph,
                                                    HANDLE file_handle,
                                                    LARGE_INTEGER file_offset,
                                                    int prot) {
  // AllocationProtect encodes the target prot for VEH demand-read.
  DWORD page_prot = windows::prot_to_page_flags(prot);

  void *base = ph.base();
  SIZE_T view_size = ph.size();
  NTSTATUS st = ph.reserve_replace(page_prot);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  // ph consumed — placeholder replaced by reserved private pages.

  // Dup the file handle for the mapping table entry.
  HANDLE dup_file = nullptr;
  if (file_handle) {
    st = ::NtDuplicateObject(NtCurrentProcess(), file_handle,
                             NtCurrentProcess(), &dup_file, 0, 0,
                             DUPLICATE_SAME_ACCESS);
    if (NT_ERROR(st)) {
      // TODO: Verify whether MEM_RELEASE alone (without MEM_PRESERVE_PLACEHOLDER)
      // correctly frees placeholder-replaced committed pages in a single step.
      // Currently using the two-step preserve→release pattern for safety.
      PVOID punch = base;
      SIZE_T punch_sz = view_size;
      ::NtFreeVirtualMemory(NtCurrentProcess(), &punch, &punch_sz,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
      // ~ph releases the punched-out placeholder via RAII.
      PlaceholderRange ph = PlaceholderRange::from_raw(base, view_size);
      (void)ph;
      return -ENOMEM;
    }
  }

  // Register with section_handle=NULL, file_handle set, VM_FLAG_FILE_PRIVATE.
  // section_offset carries the file offset for NtReadFile.
  windows::g_mapping_table.register_mapping_take(
      base, view_size,
      ViewSpec{/*section=*/nullptr, dup_file, file_offset, page_prot,
               windows::VM_FLAG_FILE_PRIVATE});

  return cpp::bit_cast<intptr_t>(base);
}

/// Replace a placeholder with private committed memory. Single syscall,
/// no section, no mapping table. Used for MAP_ANONYMOUS (non-NORESERVE).
///
/// Returns address-as-long on success, -errno on failure.
intptr_t map_anon_private_into_placeholder(PlaceholderRange &ph,
                                                       int prot) {
  void *base = ph.base();
  NTSTATUS st = ph.commit(windows::prot_to_page_flags(prot));
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);
  return cpp::bit_cast<intptr_t>(base);
}

/// Anonymous allocation with optional address hint.
///
/// Standard MAP_ANONYMOUS|MAP_PRIVATE uses one-shot allocation
/// (NtAllocateVirtualMemoryEx MEM_RESERVE|MEM_COMMIT) — one syscall,
/// 40% faster than the placeholder two-step. The kernel honors hints
/// natively and falls back to system-chosen on conflict. No oversized+split
/// needed because the hint is advisory (non-MAP_FIXED). Result is
/// MEM_PRIVATE, no mapping table entry.
///
/// PROT_NONE and MAP_NORESERVE stay on the placeholder model:
///   PROT_NONE = bare placeholder (address reservation, no commit).
///   MAP_NORESERVE = reserve-replace for demand-commit via VEH.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_hint_placeholder(void *hint, SIZE_T size, int prot,
                                         int flags) {
  // Standard MAP_ANONYMOUS|MAP_PRIVATE: one-shot allocation.
  // Honors page-aligned hints natively; the kernel rounds non-64KB-aligned
  // hints to allocation granularity — acceptable since the hint is advisory.
  if (prot != PROT_NONE && !(flags & MAP_NORESERVE)) {
    DWORD page_prot = windows::prot_to_page_flags(prot);
    void *base = nullptr;

    if (hint && windows::is_page_aligned(hint))
      base = windows::oneshot_alloc_watched(hint, size, page_prot);

    // Fall back to system-chosen address (or MAP_32BIT constrained).
    if (!base) {
      if (flags & MAP_32BIT)
        base = windows::oneshot_alloc_32bit_watched(size, page_prot);
      else
        base = windows::oneshot_alloc_watched(nullptr, size, page_prot);
    }

    if (LIBC_UNLIKELY(!base))
      return -ENOMEM;

    return cpp::bit_cast<intptr_t>(base);
  }

  // PROT_NONE and MAP_NORESERVE: placeholder model required.
  PlaceholderRange ph;

  if (hint && windows::is_page_aligned(hint)) {
    if (windows::is_alloc_aligned(hint)) {
      // 64KB-aligned: direct allocation.
      ph = PlaceholderRange::reserve(size, hint);
    } else {
      // Page-aligned but not 64KB-aligned: allocate oversized placeholder
      // at the 64KB-aligned floor, then split at the exact hint address.
      uintptr_t hint_addr = reinterpret_cast<uintptr_t>(hint);
      uintptr_t aligned = windows::align_down_to_granularity(hint_addr);
      SIZE_T prefix = hint_addr - aligned;
      // Overflow means hint can't be satisfied; fall through to system-chosen.
      if (LIBC_LIKELY(size <= SIZE_MAX - prefix)) {
        SIZE_T total = prefix + size;
        PlaceholderRange oversized = PlaceholderRange::reserve(
            total, reinterpret_cast<void *>(aligned));
        if (oversized && reinterpret_cast<uintptr_t>(oversized.base()) == aligned) {
          PlaceholderRange halves_left, halves_right;
          if (oversized.split(prefix, &halves_left, &halves_right)) {
            ph = cpp::move(halves_right);
            // halves_left (prefix) released by destructor.
          }
          // If split fails, ~oversized releases.
        }
        // If oversized was at wrong address or null, ~oversized releases.
      }
    }
  }

  // Fall back to system-chosen address (or MAP_32BIT constrained).
  if (!ph) {
    if (flags & MAP_32BIT)
      ph = PlaceholderRange::reserve_32bit(size);
    else
      ph = PlaceholderRange::reserve(size);
  }

  if (LIBC_UNLIKELY(!ph))
    return -ENOMEM;

  // PROT_NONE: pure address reservation. No section, no commit.
  if (prot == PROT_NONE)
    return cpp::bit_cast<intptr_t>(ph.consume().base);

  // MAP_NORESERVE: reserve-replace for demand-commit via VEH.
  // No section, no mapping table entry — partial munmap is trivial
  // (MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER on reserved pages).
  intptr_t result = map_anon_reserve_into_placeholder(ph, prot);
  if (LIBC_UNLIKELY(result < 0))
    return result; // ~ph releases the placeholder.
  return result;
}

/// Commit into an already-claimed placeholder (anonymous).
/// Used by NOREPLACE (placeholder claimed atomically) and alloc_fixed_free.
intptr_t alloc_fixed_free_commit(void *placeholder, SIZE_T size,
                                         int prot, int flags) {
  // Adopt ownership — destructor releases on error paths.
  PlaceholderRange ph = PlaceholderRange::from_raw(placeholder, size);

  if (prot == PROT_NONE) {
    return cpp::bit_cast<intptr_t>(ph.consume().base);
  }

  if (flags & MAP_NORESERVE) {
    intptr_t result = map_anon_reserve_into_placeholder(ph, prot);
    if (LIBC_UNLIKELY(result < 0))
      return result; // ~ph releases the placeholder.
    return result;
  }

  intptr_t result = map_anon_private_into_placeholder(ph, prot);
  if (LIBC_UNLIKELY(result < 0))
    return result; // ~ph releases the placeholder.
  return result;
}

/// MAP_FIXED into a MEM_FREE range.
///
/// Standard anonymous private at 64KB-aligned addresses uses one-shot
/// allocation — single syscall, no placeholder overhead. The kernel
/// atomically reserves and commits at the exact address.
///
/// Non-64KB-aligned addresses, PROT_NONE, and MAP_NORESERVE fall back
/// to the placeholder path (oversized+split for sub-64KB alignment).
///
/// Unlike alloc_hint_placeholder, never falls back to a system-chosen
/// address — MAP_FIXED requires the exact address or failure.
/// Returns -errno on failure (caller retries with fresh classification).
intptr_t alloc_fixed_free(void *addr, SIZE_T size, int prot,
                                  int flags) {
  // Fast path: standard anonymous private at 64KB-aligned address.
  // One-shot allocation — single syscall, no placeholder transitions.
  if (prot != PROT_NONE && !(flags & MAP_NORESERVE) &&
      windows::is_alloc_aligned(addr)) {
    DWORD page_prot = windows::prot_to_page_flags(prot);
    void *base = windows::oneshot_alloc_watched(addr, size, page_prot);
    if (LIBC_UNLIKELY(!base))
      return -ENOMEM;
    // MAP_FIXED requires exact address. If the kernel chose differently
    // (VA raced between classification and alloc), release and retry.
    if (LIBC_UNLIKELY(base != addr)) {
      windows::vm_release(base);
      return -ENOMEM;
    }
    return cpp::bit_cast<intptr_t>(base);
  }

  // Placeholder path: non-64KB-aligned, PROT_NONE, or MAP_NORESERVE.
  PlaceholderRange ph;

  if (windows::is_alloc_aligned(addr)) {
    ph = PlaceholderRange::reserve(size, addr);
  } else {
    // Page-aligned but not 64KB-aligned: oversized + split.
    uintptr_t hint_addr = reinterpret_cast<uintptr_t>(addr);
    uintptr_t aligned = windows::align_down_to_granularity(hint_addr);
    SIZE_T prefix = hint_addr - aligned;
    if (LIBC_LIKELY(size <= SIZE_MAX - prefix)) {
      SIZE_T total = prefix + size;
      PlaceholderRange oversized = PlaceholderRange::reserve(
          total, reinterpret_cast<void *>(aligned));
      if (oversized && reinterpret_cast<uintptr_t>(oversized.base()) == aligned) {
        PlaceholderRange halves_left, halves_right;
        if (oversized.split(prefix, &halves_left, &halves_right)) {
          ph = cpp::move(halves_right);
          // halves_left (prefix) released by destructor.
        }
        // If split fails, ~oversized releases.
      }
      // If oversized was at wrong address or null, ~oversized releases.
    }
  }

  if (!ph)
    return -ENOMEM; // Range occupied or VA exhausted — caller reclassifies.

  // Transfer ownership to alloc_fixed_free_commit (adopts via from_raw).
  return alloc_fixed_free_commit(ph.consume().base, size, prot, flags);
}

/// Lock-free MAP_FIXED dispatcher for anonymous mappings.
///
/// Queries MBI to classify the target range, then routes to the optimal
/// lock-free path. Falls back to alloc_fixed_split_remap only if
/// MEM_MAPPED (section) views are found in the target range.
///
/// Retry loop: if a sub-function fails and the region's MBI classification
/// has changed (concurrent mmap/munmap/mremap), re-queries and re-dispatches.
/// Handles stale classification from the TOCTOU window between query and
/// dispatch without a global lock.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_fixed_anon(void *addr, SIZE_T size, int prot,
                                  int flags) {
  constexpr int MAX_CLASSIFY_RETRIES = 4;

  for (int attempt = 0; attempt < MAX_CLASSIFY_RETRIES; ++attempt) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!windows::query_region(addr, mbi))
      return -EINVAL;

    DWORD orig_state = mbi.State;
    DWORD orig_type = mbi.Type;
    void *orig_alloc = mbi.AllocationBase;
    intptr_t result = -ENOMEM;

    if (mbi.State == MEM_FREE) {
      // Free VA: create placeholder at exact address, no fallback.
      result = alloc_fixed_free(addr, size, prot, flags);
    } else if (mbi.Type == MEM_MAPPED) {
      // Section view — needs lock+guard split-remap path.
      result = alloc_fixed_split_remap(addr, size, prot, flags);
    } else if (mbi.Type == MEM_IMAGE) {
      // Cannot replace executable images — hard error, no retry.
      return -EINVAL;
    } else {
      // MEM_PRIVATE: lock-free path.
      char *ab = static_cast<char *>(mbi.AllocationBase);
      char *target_start = static_cast<char *>(addr);
      char *target_end = target_start + size;

      char *alloc_end = windows::find_alloc_end(mbi.AllocationBase);

      if (target_start >= ab && target_end <= alloc_end)
        result = alloc_fixed_private_placeholder(addr, size, prot);
      else if (target_start == ab && target_end == alloc_end)
        result = alloc_fixed_private_placeholder(addr, size, prot);
      else
        result = alloc_fixed_private_multi(addr, size, prot);
    }

    if (result >= 0)
      return result;

    // Sub-function failed. Re-query to distinguish stale classification
    // (concurrent mutation changed the region) from a real error.
    MEMORY_BASIC_INFORMATION mbi2;
    if (!windows::query_region(addr, mbi2))
      return -EINVAL; // Address space gone.

    bool changed = mbi2.State != orig_state ||
                   mbi2.Type != orig_type ||
                   mbi2.AllocationBase != orig_alloc;
    if (!changed)
      return result; // Same classification — real error, propagate.

    // Classification changed — retry dispatch with fresh state.
    ::NtYieldExecution();
  }

  return -ENOMEM;
}

} // namespace

// ===========================================================================
// Kernel function — implements Linux SYS_mmap semantics in userspace.
// Returns the mapped address (as long) on success, -errno on failure.
// Called from syscall_impl() dispatch and from the POSIX entry point below.
//
// All helpers use the address-as-long / -errno convention consistently.
// Direct validation errors and helper failures both return -errno.
// ===========================================================================

namespace internal {

intptr_t mmap(void *addr, size_t size, int prot, int flags, int fd,
              off_t offset) {
  if (LIBC_UNLIKELY(size == 0))
    return -EINVAL;

  // POSIX requires exactly one of MAP_SHARED or MAP_PRIVATE.
  // Both set: Linux returns EINVAL; we follow suit.
  // Neither set: also EINVAL.
  const int sharing = flags & (MAP_SHARED | MAP_PRIVATE);
  if (LIBC_UNLIKELY(sharing == 0 ||
                    sharing == (MAP_SHARED | MAP_PRIVATE)))
    return -EINVAL;

  // W^X: reject simultaneous W+X unless the caller explicitly opts in with
  // MAP_WX.  The correct JIT pattern is mmap(RW) → write → mprotect(RX).
  if (LIBC_UNLIKELY((prot & PROT_WRITE) && (prot & PROT_EXEC) &&
                    !(flags & MAP_WX)))
    return -EACCES;

  const SIZE_T rounded_size = windows::round_to_page(size);
  if (LIBC_UNLIKELY(rounded_size == 0))
    return -ENOMEM;

  // --- MAP_FIXED_NOREPLACE: atomic VA claim ---
  //
  // Standard anonymous private at 64KB-aligned addresses uses one-shot
  // allocation for atomic claim: oneshot_alloc at the exact address. If the
  // kernel returns a different address, the VA was occupied → EEXIST. This
  // is handled inline in the anonymous branch below (noreplace_oneshot=true).
  //
  // All other cases (non-64KB-aligned, PROT_NONE, MAP_NORESERVE, file-backed)
  // use placeholder atomic claim: NtAllocateVirtualMemoryEx with
  // MEM_RESERVE_PLACEHOLDER at the exact address. Failure → EEXIST.
  //
  // HUGETLB exception: large-page paths manage their own placeholder
  // lifecycle internally. Fall back to is_range_free (best-effort).
  PlaceholderRange noreplace_ph;
  bool noreplace_oneshot = false;
  if (flags & MAP_FIXED_NOREPLACE) {
    if (LIBC_UNLIKELY(!addr || !windows::is_page_aligned(addr)))
      return -EINVAL;

    if (flags & MAP_HUGETLB) {
      // HUGETLB manages its own placeholder — best-effort free check.
      if (!windows::is_range_free(addr, rounded_size))
        return -EEXIST;
      flags |= MAP_FIXED;
    } else if ((flags & MAP_ANONYMOUS) && prot != PROT_NONE &&
               !(flags & MAP_NORESERVE) &&
               windows::is_alloc_aligned(addr)) {
      // Standard anonymous private at 64KB-aligned: one-shot handles
      // NOREPLACE atomically. Deferred to the anonymous branch below.
      noreplace_oneshot = true;
    } else {
      // Atomic claim via placeholder for all other cases.
      PlaceholderRange claim;
      if (windows::is_alloc_aligned(addr)) {
        claim = PlaceholderRange::reserve(rounded_size, addr);
      } else {
        // Non-64KB-aligned: oversized placeholder + split.
        uintptr_t hint_addr = reinterpret_cast<uintptr_t>(addr);
        uintptr_t aligned = windows::align_down_to_granularity(hint_addr);
        SIZE_T prefix = hint_addr - aligned;
        if (LIBC_LIKELY(rounded_size <= SIZE_MAX - prefix)) {
          SIZE_T total = prefix + rounded_size;
          PlaceholderRange oversized = PlaceholderRange::reserve(
              total, reinterpret_cast<void *>(aligned));
          if (oversized && reinterpret_cast<uintptr_t>(oversized.base()) == aligned) {
            PlaceholderRange halves_left, halves_right;
            if (oversized.split(prefix, &halves_left, &halves_right)) {
              claim = cpp::move(halves_right);
              // halves_left (prefix) released by destructor.
            }
            // If split fails, ~oversized releases.
          }
          // If oversized was at wrong address or null, ~oversized releases.
        }
      }

      if (!claim)
        return -EEXIST;
      // Claim succeeded — transfer ownership for downstream use.
      noreplace_ph = cpp::move(claim);
      // Fall through to commit in anon/file branch.
    }
  }

  // --- Large page allocation (MAP_HUGETLB) ---
  // Section-backed with SEC_LARGE_PAGES for both anonymous and file-backed.
  // Same placeholder model as regular mappings. Requires SeLockMemoryPrivilege.
  if (flags & MAP_HUGETLB) {
    intptr_t result;
    if (flags & MAP_ANONYMOUS) {
      result = alloc_large_pages(addr, rounded_size, prot, flags);
    } else {
      internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
      if (LIBC_UNLIKELY(!ofd))
        return -EBADF;
      if (LIBC_UNLIKELY(ofd->is_path_only()))
        return -EBADF;
      int oflags = ofd->access_mode |
                   ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
      if (int err = validate_file_prot(prot, flags, oflags))
        return -static_cast<intptr_t>(err);
      HANDLE handle = ofd->handle;
      result = alloc_file_large_pages(addr, rounded_size, prot, flags, handle,
                                      offset, oflags, fd);
      // Registration handled inside alloc_file_large_pages.
    }
    return result;
  }

  // --- Anonymous mapping ---
  // Standard MAP_ANONYMOUS|MAP_PRIVATE uses one-shot allocation (single syscall).
  // PROT_NONE and MAP_NORESERVE use placeholder model (demand-commit via VEH).
  // MAP_SHARED|MAP_ANONYMOUS treated as private on Windows (no fork).
  if (flags & MAP_ANONYMOUS) {
    intptr_t result;

    if (noreplace_oneshot) {
      // NOREPLACE via one-shot: atomic reserve+commit at exact address.
      // If the kernel returns a different address, the VA was occupied.
      DWORD page_prot = windows::prot_to_page_flags(prot);
      void *base = windows::oneshot_alloc_watched(addr, rounded_size, page_prot);
      if (LIBC_UNLIKELY(!base))
        return -EEXIST;
      if (LIBC_UNLIKELY(base != addr)) {
        windows::vm_release(base);
        return -EEXIST;
      }
      result = cpp::bit_cast<intptr_t>(base);
    } else if (noreplace_ph) {
      // NOREPLACE via placeholder: already claimed atomically. Commit into it.
      result = alloc_fixed_free_commit(noreplace_ph.base(), rounded_size, prot,
                                       flags);
      if (LIBC_UNLIKELY(result < 0))
        return result; // ~noreplace_ph releases
      (void)noreplace_ph.consume(); // consumed by commit
    } else if (flags & MAP_FIXED) {
      if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
        return -EINVAL;

      // Lock-free MAP_FIXED: dispatch based on target region type.
      // MEM_PRIVATE uses kernel-atomic placeholder CAS — no locks, no VEH.
      // MEM_MAPPED falls back to split-remap with lock+guard.
      result = alloc_fixed_anon(addr, rounded_size, prot, flags);

      if (LIBC_UNLIKELY(result < 0))
        return result;
    } else {
      result = alloc_hint_placeholder(addr, rounded_size, prot, flags);
      if (LIBC_UNLIKELY(result < 0))
        return result;
    }

    // MAP_POPULATE: prefault committed pages into the working set.
    // MAP_POPULATE triggers SEC_COMMIT (above), so all pages are committed.
    if ((flags & MAP_POPULATE) && prot != PROT_NONE)
      windows::prefetch_committed(reinterpret_cast<void *>(result),
                                          rounded_size);

    // MAP_LOCKED: lock pages immediately after mapping.
    if ((flags & MAP_LOCKED) && prot != PROT_NONE)
      windows::lock_range(reinterpret_cast<void *>(result), rounded_size);

    // MCL_FUTURE: lock newly committed pages if mlockall(MCL_FUTURE) active.
    if (prot != PROT_NONE)
      windows::lock_if_future(reinterpret_cast<void *>(result), rounded_size);

    return result;
  }

  // --- File-backed mapping ---
  {
    internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
    if (LIBC_UNLIKELY(!ofd))
      return -EBADF; // ~noreplace_ph releases if non-empty
    if (LIBC_UNLIKELY(ofd->is_path_only()))
      return -EBADF;
    HANDLE handle = ofd->handle;
    int open_flags = ofd->access_mode |
                     ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);

    if (int err = validate_file_prot(prot, flags, open_flags))
      return -static_cast<intptr_t>(err); // ~noreplace_ph releases

    if (LIBC_UNLIKELY(
            offset < 0 ||
            (static_cast<SIZE_T>(offset) & (windows::get_page_size() - 1))))
      return -EINVAL; // ~noreplace_ph releases

    intptr_t result;
    if (noreplace_ph) {
      // NOREPLACE: placeholder already claimed. Map file into it.
      const DWORD64 aligned_offset =
          windows::round_down_to_page_offset(static_cast<DWORD64>(offset));
      LARGE_INTEGER section_offset;
      section_offset.QuadPart = static_cast<LONGLONG>(aligned_offset);

      if (flags & MAP_PRIVATE) {
        // Private file: reserve-replace + demand-read via VEH.
        result = map_file_private_into_placeholder(
            noreplace_ph, handle, section_offset, prot);
        if (LIBC_UNLIKELY(result < 0))
          return result; // ~noreplace_ph releases
      } else {
        // Shared file: section view.
        HANDLE out_section = nullptr;
        DWORD out_prot = 0;
        result = map_file_into_placeholder(noreplace_ph, handle,
                                           section_offset, prot, flags,
                                           open_flags, fd, &out_section,
                                           &out_prot);
        if (LIBC_UNLIKELY(result < 0))
          return result; // ~noreplace_ph releases
        HANDLE dup_file = nullptr;
        if (handle) {
          NTSTATUS dup_st = ::NtDuplicateObject(
              NtCurrentProcess(), handle, NtCurrentProcess(), &dup_file, 0, 0,
              DUPLICATE_SAME_ACCESS);
          if (NT_ERROR(dup_st)) {
            ::NtClose(out_section);
            ::NtUnmapViewOfSectionEx(NtCurrentProcess(),
                                     reinterpret_cast<void *>(result), 0);
            return -ENOMEM;
          }
        }
        windows::g_mapping_table.register_mapping_take(
            reinterpret_cast<void *>(result), rounded_size,
            ViewSpec{out_section, dup_file, section_offset, out_prot,
                     /*flags=*/0});
      }
    } else if (flags & MAP_FIXED) {
      if (LIBC_UNLIKELY(!windows::is_page_aligned(addr)))
        return -EINVAL;
      result = alloc_file_fixed(addr, rounded_size, prot, flags, handle,
                                offset, open_flags, fd);
    } else {
      result = alloc_file_hint(addr, rounded_size, prot, flags, handle,
                               offset, open_flags, fd);
    }

    if (LIBC_UNLIKELY(result < 0))
      return result;

    // Registration (section handle, file handle, offset, protection) is
    // handled inside alloc_file_fixed / alloc_file_hint.

    // MAP_POPULATE: prefault file-backed pages into the working set.
    if (flags & MAP_POPULATE)
      windows::prefetch_committed(reinterpret_cast<void *>(result),
                                          rounded_size);

    // MAP_LOCKED: lock pages immediately after mapping.
    if (flags & MAP_LOCKED)
      windows::lock_range(reinterpret_cast<void *>(result), rounded_size);

    // MCL_FUTURE: lock newly mapped file pages if mlockall(MCL_FUTURE) active.
    windows::lock_if_future(reinterpret_cast<void *>(result), rounded_size);

    return result;
  }
}

} // namespace internal

} // namespace LIBC_NAMESPACE_DECL

// Reset g_mmap_lock in fork child. Non-atomic set clears both
// the value and queue metadata. Child is single-threaded; no parent
// waiters exist. Without this, a parent-held lock deadlocks the child.
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
void LIBC_NAMESPACE::internal::mmap_lock_fork_reinit() {
  LIBC_NAMESPACE::windows::g_mmap_lock.fork_reinit();
}
