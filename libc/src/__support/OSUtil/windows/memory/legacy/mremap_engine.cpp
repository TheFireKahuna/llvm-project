//===---------- Windows mremap engine (kernel function) --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// mremap on Windows via the section-backed placeholder model.
//
// Under the universal section model, all POSIX-visible mmap allocations use
// placeholder -> section -> view. mremap operates on the underlying section
// directly, avoiding memcpy for all section-backed paths.
//
// Section growth:
//   File-backed: NtExtendSection grows the file and section. O(1).
//   Pagefile-backed: NtExtendSection rejects with STATUS_SECTION_NOT_EXTENDED
//     (there is no backing file to grow). A new section is created at the
//     target size, data is copied via a temporary view, and the new section
//     replaces the old one.
//
// Paths by operation:
//
//   Shrink (new_size < old_size):
//     Unmap view -> split placeholder -> remap with smaller view_size.
//     Same section preserves content. Protection variations replayed.
//
//   Grow with move (MREMAP_MAYMOVE):
//     Atomic move using TLB shootdown for Linux-equivalent semantics:
//       begin_remap_guard -> NtProtectVirtualMemory(PAGE_NOACCESS) ->
//       NtMapViewOfSectionEx(new) -> NtUnmapViewOfSectionEx(old) ->
//       abort_remap_guard
//     NtProtectVirtualMemory issues a cross-CPU IPI that flushes TLB
//     entries, eliminating the Phase 1 readable window. Combined with
//     the VEH remap guard, this matches Linux mmap_write_lock semantics.
//
//   Grow in-place (no MREMAP_MAYMOVE, or tried before move):
//     If [base+old_size, base+new_size) is MEM_FREE, coalesces the
//     old view's placeholder with a new extension placeholder, then
//     remaps the grown section as a single view. No TLB shootdown
//     needed -- same base address, no stale-pointer scenario. VEH
//     guard stalls concurrent faulters during the ~1.5us window.
//     Returns ENOMEM if adjacent VA is occupied.
//
//   MREMAP_FIXED:
//     Same atomic pattern targeting a specific address. Uses the
//     global FixedMapLock to serialize with concurrent MAP_FIXED.
//
//   MREMAP_DONTUNMAP:
//     After moving the view, maps a pre-created zero-filled section at
//     old_addr (Linux: old range stays accessible, faults in zeroes).
//     The fresh section is created before any destructive operation so
//     failure is fail-fast, not best-effort.
//
// Bare placeholders (PROT_NONE mmap not yet demand-mapped via mprotect)
// use simple placeholder split/release -- no section operations needed.
//
// Internal bookkeeping (COW save buffers) uses the same placeholder ->
// section -> view model per the universal placeholder design.
//
// Engine function: returns address-as-long on success, -errno on failure.
// Never sets libc_errno — that is the entry point's responsibility.
//
//===----------------------------------------------------------------------===//

#include "mremap_engine.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/remap_transaction.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_classifier.h"
#include "src/__support/OSUtil/windows/memory/legacy/view_spec.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_snapshot.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

//==========================================================================
// Multi-view collection for copyless mremap move
//==========================================================================
//
// After grow-in-place, a logical mapping may span multiple adjacent section
// views (e.g., original + extension). move_section must relocate ALL views,
// not just the first. Collect them upfront by walking the address range.
//
// Each slice carries a held region reference (transferred from extract()).
// The slice borrows section/file/offset from its region so map_into() can
// build a transient ViewSpec without re-resolving. If the slice is later
// re-registered at a new address, register_mapping consumes the held ref;
// otherwise the destruction path (release_view_slices) drops it.

/// A single section view within a multi-view logical mapping.
struct ViewSliceEntry {
  uint32_t region_id;     // held ref; release on drop or re-register
  uint8_t alloc_id;
  DWORD prot;
  DWORD flags;
  HANDLE section;         // borrowed from region (region keeps ownership)
  HANDLE file;            // borrowed
  LARGE_INTEGER offset;   // adjusted offset for this slice
  SIZE_T size;            // bytes covered by this slice
};

constexpr int MAX_VIEW_SLICES = 16;

LIBC_INLINE windows::ViewSpec view_spec_from_slice(const ViewSliceEntry &s) {
  windows::ViewSpec v{};
  v.section = s.section;
  v.file = s.file;
  v.offset = s.offset;
  v.prot = s.prot;
  v.flags = s.flags;
  return v;
}

/// Walk [addr, addr+total_size) and extract every MEM_MAPPED slot. Each
/// extracted slot transfers its region reference into the corresponding
/// ViewSliceEntry. On premature termination, already-collected references
/// are released so the caller does not have to special-case partial walks.
int collect_view_slices(void *addr, SIZE_T total_size, ViewSliceEntry *out,
                        int max_slices) {
  char *base_addr = static_cast<char *>(addr);
  char *end = base_addr + total_size;
  int count = 0;

  // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
  // which is what we want — extract() touches the mapping table, not VA.
  auto ws = windows::byte_scratch(4096);
  if (!ws)
    return 0;
  nt_pal::RegionWalker walk(addr, total_size, ws.data(), ws.size());

  // `last_consumed_end` tracks the high-water mark of consumed view extents
  // so multi-MBI views (rare today, AllocationBase == view_base) are skipped
  // on subsequent iterations. Mirrors the old loop's `cur = view_end` skip.
  char *last_consumed_end = base_addr;

  while (walk.next() && count < max_slices) {
    char *region_base = static_cast<char *>(walk.entry->BaseAddress);
    if (region_base < last_consumed_end)
      continue;

    if (walk.entry->State != MEM_FREE && walk.entry->Type == MEM_MAPPED &&
        region_base == walk.entry->AllocationBase) {
      windows::MappingEntry entry{};
      if (windows::g_mapping_table.extract(region_base, &entry)) {
        windows::memory::RegionDesc *rd =
            windows::memory::g_region_pool.get_mutable(entry.region_id);
        if (rd != nullptr && rd->section_handle != nullptr) {
          char *view_end = static_cast<char *>(entry.view_base) +
                           entry.view_size;
          if (view_end > end)
            view_end = end;

          // Section offset for the slice. region_base should equal
          // entry.view_base at AllocationBase; the addend covers any future
          // CHUNKED slot whose first chunk does not start at the region origin.
          LARGE_INTEGER offset = rd->section_offset;
          offset.QuadPart += static_cast<LONGLONG>(
              region_base - static_cast<char *>(entry.view_base));

          out[count].region_id = entry.region_id;
          out[count].alloc_id = entry.alloc_id;
          out[count].prot = entry.view_prot;
          out[count].flags = entry.flags;
          out[count].section = rd->section_handle;
          out[count].file = rd->file_handle;
          out[count].offset = offset;
          out[count].size = static_cast<SIZE_T>(view_end - region_base);
          ++count;
          last_consumed_end = view_end;
          continue;
        }
        // Slot extracted but the region is unusable — drop the held ref so
        // the slice list never carries stale region IDs.
        if (entry.region_id != windows::memory::RegionPool::NONE)
          windows::memory::g_region_pool.release(entry.region_id);
      }
    }
  }

  return count;
}

/// Release the region reference held by every slice. Used on abort paths
/// where the slice was extracted but its slot will not be re-published.
void release_view_slices(ViewSliceEntry *slices, int count) {
  for (int i = 0; i < count; ++i) {
    if (slices[i].region_id != windows::memory::RegionPool::NONE) {
      windows::memory::g_region_pool.release(slices[i].region_id);
      slices[i].region_id = windows::memory::RegionPool::NONE;
    }
  }
}

/// Map a collected view-slice array into a target placeholder.
/// The placeholder at `target` must be at least `total_size` bytes.
/// Splits the placeholder at each slice boundary and maps each section.
/// If growing (extra_section != null), appends the extension as the last
/// slice. Returns true on success.
///
/// On failure, unmaps any already-mapped slices and leaves the placeholder
/// in a partially-split state (caller must clean up).
bool map_slices_into_placeholder(
    HANDLE process, void *target, ViewSliceEntry *slices, int slice_count,
    SIZE_T total_old_size, HANDLE extra_section, SIZE_T extra_size,
    DWORD extra_prot) {
  char *base = static_cast<char *>(target);
  SIZE_T total = total_old_size + extra_size;

  // Split placeholder at each internal boundary. After each split,
  // the remaining placeholder starts at base + split_offset. The next
  // split operates on the REMAINING placeholder, not the original.
  char *split_base = base;
  for (int i = 0; i < slice_count - 1; ++i) {
    if (!nt_pal::split_placeholder(split_base, slices[i].size))
      return false;
    split_base += slices[i].size;
  }

  // Split before extension if present.
  if (extra_section && total_old_size < total) {
    if (!nt_pal::split_placeholder(split_base, slices[slice_count - 1].size))
      return false;
  }

  // Map each slice.
  int mapped = 0;
  for (int i = 0; i < slice_count; ++i) {
    windows::ViewSpec spec = view_spec_from_slice(slices[i]);
    NTSTATUS st = spec.map_into(base, slices[i].size);
    if (NT_ERROR(st)) {
      // Unmap already-mapped slices.
      char *undo = static_cast<char *>(target);
      for (int j = 0; j < mapped; ++j) {
        nt_pal::unmap_view(undo);
        undo += slices[j].size;
      }
      return false;
    }
    base += slices[i].size;
    ++mapped;
  }

  // Map extension.
  if (extra_section) {
    PVOID ext_base = base;
    SIZE_T ext_size = extra_size;
    LARGE_INTEGER ext_off = {};
    NTSTATUS st = ::NtMapViewOfSectionEx(
        extra_section, process, &ext_base, &ext_off, &ext_size,
        MEM_REPLACE_PLACEHOLDER, extra_prot, nullptr, 0);
    if (NT_ERROR(st)) {
      // Unmap all old slices.
      char *undo = static_cast<char *>(target);
      for (int j = 0; j < mapped; ++j) {
        nt_pal::unmap_view(undo);
        undo += slices[j].size;
      }
      return false;
    }
  }

  return true;
}

/// Acquire a fresh region for an extension section (pagefile-backed,
/// SEC_RESERVE, demand-commit via VEH). Takes ownership of `section` on
/// success; on failure the caller retains the handle.
///
/// Shape is `ANON_RESERVE_SECTION` — the section provides growth capacity
/// for an mremap, but there is no fd backing, so msync / mincore /
/// /proc-style reporting must continue to treat the range as anonymous.
[[nodiscard]] uint32_t acquire_extension_region(HANDLE section, void *base,
                                                SIZE_T size, DWORD prot) {
  windows::memory::AcquireSpec spec{};
  spec.section_handle = section;
  spec.file_handle = nullptr;
  spec.section_offset = LARGE_INTEGER{};
  spec.shape = windows::memory::RegionShape::ANON_RESERVE_SECTION;
  spec.flags = windows::memory::region_flag::NORESERVE;
  spec.first_slot_key =
      reinterpret_cast<uintptr_t>(base) >> 16;
  spec.last_slot_key = spec.first_slot_key + (size >> 16);
  (void)prot;
  return windows::memory::g_region_pool.acquire_owned(spec);
}

/// Register all slices + optional extension in the mapping table.
///
/// Each slice's region reference is consumed by register_mapping on success.
/// On failure, the unsubmitted reference is released and a partial publish
/// is rolled back by releasing every slice (the caller already unmapped or
/// will unmap the underlying NT views).
///
/// `extra_region_id` is the region acquired for the extension section (or
/// NONE if no extension). On success it is consumed; on failure it is
/// released here.
bool register_view_slices(void *target, ViewSliceEntry *slices, int count,
                          uint32_t extra_region_id, DWORD extra_prot,
                          SIZE_T extra_size = 0) {
  char *base = static_cast<char *>(target);
  for (int i = 0; i < count; ++i) {
    bool ok = windows::g_mapping_table.register_mapping(
        base, slices[i].size, slices[i].region_id, slices[i].alloc_id,
        slices[i].prot, slices[i].flags);
    if (!ok) {
      // Unwind: release this slice's ref and every still-held downstream
      // ref. The previously-published slices keep their refs (they are
      // now slot-owned again).
      windows::memory::g_region_pool.release(slices[i].region_id);
      slices[i].region_id = windows::memory::RegionPool::NONE;
      for (int j = i + 1; j < count; ++j) {
        if (slices[j].region_id != windows::memory::RegionPool::NONE) {
          windows::memory::g_region_pool.release(slices[j].region_id);
          slices[j].region_id = windows::memory::RegionPool::NONE;
        }
      }
      if (extra_region_id != windows::memory::RegionPool::NONE)
        windows::memory::g_region_pool.release(extra_region_id);
      return false;
    }
    slices[i].region_id = windows::memory::RegionPool::NONE; // consumed
    base += slices[i].size;
  }
  if (extra_region_id != windows::memory::RegionPool::NONE) {
    LARGE_INTEGER ext_off = {};
    (void)ext_off;
    if (!windows::g_mapping_table.register_mapping(
            base, extra_size, extra_region_id,
            windows::memory::g_region_pool.get_mutable(extra_region_id)
                ->alloc_id,
            extra_prot, /*flags=*/0)) {
      windows::memory::g_region_pool.release(extra_region_id);
      return false;
    }
  }
  return true;
}

// Aliases for types used throughout this file.
using windows::PlaceholderRange;
using windows::RegionRecord;
using windows::MAX_REGION_RECORDS;

//==========================================================================
// Section view shrink
//==========================================================================
//
// Unmap -> split placeholder -> remap with smaller view_size.
// Works for both anonymous and file-backed section views.
// Same section preserves content; protection snapshot/replay handles
// sub-region mprotect variations.
//
// Returns address-as-long on success, -errno on failure.

intptr_t shrink_section_view(void *addr, SIZE_T old_size,
                                      SIZE_T new_size) {
  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  void *view_end = static_cast<char *>(addr) + old_size;

  // Target = the tail being removed. Left fragment = kept prefix.
  windows::RemapTransaction txn(addr, view_end,
                                addr_val + new_size,
                                old_size - new_size);

  if (!txn.prepare())
    return -EINVAL;

  if (txn.region() == nullptr ||
      txn.region()->section_handle == nullptr) {
    // ~txn handles cleanup.
    return -EINVAL;
  }

  auto result = txn.execute();
  if (!result.target)
    return -ENOMEM; // ~txn rolls back.

  // Release the tail placeholder (freeing the VA range).
  // consume() transfers ownership out; nt_pal::free_placeholder() frees the VA.
  auto [tail_base, tail_size] = result.target.consume();
  nt_pal::free_placeholder(tail_base);

  if (!result.left_ok) {
    // Prefix remap failed — can't shrink.
    return -ENOMEM; // ~txn rolls back remaining state.
  }

  if (!txn.commit())
    return -ENOMEM;
  return static_cast<intptr_t>(addr_val);
}

//==========================================================================
// Atomic section move (TLB shootdown)
//==========================================================================
//
// NtProtectVirtualMemory(PAGE_NOACCESS) issues a cross-CPU TLB shootdown
// IPI -- the same hardware primitive Linux uses in flush_tlb_mm_range under
// mmap_write_lock. This eliminates the Phase 1 readable window where
// old_addr could be accessed after mremap begins.
//
// Sequence:
//   0. Pre-create fresh section for DONTUNMAP (fail-fast)
//   1. Extend section (if growing)
//   2. Arm remap guard at old_addr
//   3. Snapshot protections + save COW content
//   4. TLB shootdown: NtProtectVirtualMemory(old_addr, PAGE_NOACCESS)
//   5. Create placeholder at target (FixedMapLock if MREMAP_FIXED)
//   6. Map section at new_addr (MEM_REPLACE_PLACEHOLDER)
//   7. Unmap old view (MEM_PRESERVE_PLACEHOLDER)
//   8. Replay protections + restore COW at new_addr
//   9. DONTUNMAP: map pre-created section at old_addr (rollback on failure)
//  10. Disarm guard, register mapping
//
// The DONTUNMAP section is created in step 0 -- before any destructive
// operation -- so resource exhaustion fails fast. The map into old_addr's
// placeholder in step 9 targets a placeholder we own with a section we
// pre-created; failure indicates a serious internal error and triggers
// full rollback (unmap new, remap old, release new placeholder).
//
// Returns address-as-long on success, -errno on failure.

intptr_t move_section(void *old_addr, SIZE_T old_size,
                               SIZE_T new_size, void *fixed_addr,
                               bool dontunmap) {
  HANDLE process = NtCurrentProcess();

  // Arm the remap guard before extraction so the VEH can stall faults
  // while entries are being cleared. Without this, a demand-commit
  // fault on a SEC_RESERVE page between extraction and guard creation
  // would see no entry and no guard, delivering a spurious SIGSEGV (N2).
  if (!windows::g_mapping_table.begin_remap_guard(old_addr, old_size))
    return -ENOMEM;

  // ── Step 0: Collect all views in [old_addr, old_addr+old_size) ──
  // After grow-in-place, the logical mapping may span multiple adjacent
  // section views. Collect them all for a copyless move.
  ViewSliceEntry slices[MAX_VIEW_SLICES];
  int slice_count =
      collect_view_slices(old_addr, old_size, slices, MAX_VIEW_SLICES);
  if (slice_count == 0) {
    windows::g_mapping_table.abort_remap_guard(old_addr);
    return -EINVAL;
  }

  // First slice determines file-backed vs pagefile and the base view_prot.
  DWORD base_prot = slices[0].prot;
  // ── Step 1: Pre-create sections for grow + DONTUNMAP (fail-fast) ──
  HANDLE ext_section = nullptr;
  HANDLE fresh_section = nullptr;
  // ScopeGuards close pre-created sections on any early-return error path.
  // Dismissed on the success path after handles are transferred to the
  // mapping table (extension via acquire_extension_region; fresh via the
  // direct register_mapping below).
  auto ext_guard = cpp::make_scope_guard([&] {
    if (ext_section)
      ::NtClose(ext_section);
  });
  auto fresh_guard = cpp::make_scope_guard([&] {
    if (fresh_section)
      ::NtClose(fresh_section);
  });
  SIZE_T delta = (new_size > old_size) ? new_size - old_size : 0;

  if (delta > 0) {
    // Always use adjacent-view for pagefile grow. For file-backed single
    // view, NtExtendSection would have already produced a single larger
    // view (handled by grow_section_in_place). If we're in move_section
    // with a single file-backed view that needs growing, extend it.
    if (slice_count == 1) {
      // Try extend (works for file-backed, fails for pagefile).
      SIZE_T ext_max = static_cast<SIZE_T>(slices[0].offset.QuadPart) +
                       new_size;
      NTSTATUS ext_st = nt_helpers::extend_section(slices[0].section,
                                                   ext_max);
      if (NT_SUCCESS(ext_st)) {
        // File-backed extend succeeded. Treat as single view at new_size.
        slices[0].size = new_size;
        delta = 0; // No extension section needed.
      }
    }

    // If still growing (pagefile or extend failed): create extension section.
    if (delta > 0) {
      LARGE_INTEGER ext_sec_size;
      ext_sec_size.QuadPart = static_cast<LONGLONG>(delta);
      auto ext_oa = windows::internal_oa();
      NTSTATUS st = ::NtCreateSectionEx(
          &ext_section,
          SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
              SECTION_QUERY,
          &ext_oa, &ext_sec_size, PAGE_EXECUTE_READWRITE, SEC_RESERVE,
          nullptr, nullptr, 0);
      if (NT_ERROR(st)) {
        windows::g_mapping_table.abort_remap_guard(old_addr);
        // Slices were extracted; restore them to the table before bailing.
        register_view_slices(old_addr, slices, slice_count,
                             windows::memory::RegionPool::NONE, 0);
        release_view_slices(slices, slice_count);
        return -ENOMEM;
      }
    }
  }

  // For DONTUNMAP we install a fresh anonymous pagefile-backed section over
  // the old VA so the user retains accessible storage there. The reservation
  // semantics of the fresh section must match the source: NORESERVE source
  // ⇒ SEC_RESERVE fresh (demand-commit, no eager pagefile reservation);
  // committed source ⇒ SEC_COMMIT fresh (eager). Track the choice so the
  // post-map publish step picks the matching shape + flags.
  bool fresh_noreserve = false;
  if (dontunmap) {
    if (slice_count > 0 &&
        slices[0].region_id != windows::memory::RegionPool::NONE) {
      windows::memory::RegionDesc *src_rd =
          windows::memory::g_region_pool.get_mutable(slices[0].region_id);
      if (src_rd != nullptr)
        fresh_noreserve = src_rd->has_flag(
            windows::memory::region_flag::NORESERVE);
    }
    LARGE_INTEGER sec_size;
    sec_size.QuadPart = static_cast<LONGLONG>(old_size);
    auto fresh_oa = windows::internal_oa();
    const ULONG fresh_sec_attr = fresh_noreserve ? SEC_RESERVE : SEC_COMMIT;
    NTSTATUS st = ::NtCreateSectionEx(
        &fresh_section,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
            SECTION_QUERY,
        &fresh_oa, &sec_size, PAGE_EXECUTE_READWRITE, fresh_sec_attr, nullptr,
        nullptr, 0);
    if (NT_ERROR(st)) {
      windows::g_mapping_table.abort_remap_guard(old_addr);
      register_view_slices(old_addr, slices, slice_count,
                           windows::memory::RegionPool::NONE, 0);
      release_view_slices(slices, slice_count);
      return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(st));
      // ~ext_guard + ~fresh_guard close pre-created sections.
    }
  }

  uintptr_t old_val = reinterpret_cast<uintptr_t>(old_addr);

  // ── Step 3: Unified snapshot + COW writeback ──
  auto records = internal::ScratchAlloc<RegionRecord>(MAX_REGION_RECORDS);
  if (!records) {
    windows::g_mapping_table.abort_remap_guard(old_addr);
    register_view_slices(old_addr, slices, slice_count,
                         windows::memory::RegionPool::NONE, 0);
    release_view_slices(slices, slice_count);
    return -ENOMEM;
  }
  int rec_count = windows::snapshot_regions(old_val, old_val + old_size,
                                            records.data(),
                                            MAX_REGION_RECORDS);
  if (LIBC_UNLIKELY(rec_count < 0)) {
    windows::g_mapping_table.abort_remap_guard(old_addr);
    register_view_slices(old_addr, slices, slice_count,
                         windows::memory::RegionPool::NONE, 0);
    release_view_slices(slices, slice_count);
    return -ENOMEM;
    // ~ext_guard + ~fresh_guard close pre-created sections.
  }

  // WSEX per-page COW scan + save dirty page content.
  windows::CowContext cow_ctx;
  if (!cow_ctx.prepare(old_val, records.data(), rec_count)) {
    windows::g_mapping_table.abort_remap_guard(old_addr);
    register_view_slices(old_addr, slices, slice_count,
                         windows::memory::RegionPool::NONE, 0);
    release_view_slices(slices, slice_count);
    return -ENOMEM;
    // ~ext_guard + ~fresh_guard close pre-created sections.
  }

  // ── Step 4: TLB shootdown — PAGE_NOACCESS on all views ──
  // NtProtectVirtualMemory operates per-allocation-region. For multi-view
  // mappings, protect each view separately. The cross-CPU IPI on each call
  // flushes TLB entries for that range.
  {
    // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
    // which is what we want — PAGE_NOACCESS doesn't move regions.
    auto ws = windows::byte_scratch(4096);
    if (ws) {
      nt_pal::RegionWalker walk(old_addr, old_size, ws.data(), ws.size());
      while (walk.next()) {
        if (walk.entry->State == MEM_COMMIT) {
          PVOID pb = walk.chunk;
          SIZE_T ps = walk.chunk_size;
          ULONG old_prot;
          ::NtProtectVirtualMemory(process, &pb, &ps, PAGE_NOACCESS,
                                   &old_prot);
        }
      }
    }
    // Scratch failure: skip TLB shootdown — VEH remap guard still stalls
    // concurrent faulters; the readable-window cost is taken but the
    // operation remains correct.

    // ARM64: NtProtectVirtualMemory issues a TLB shootdown IPI on x86-64,
    // but ARM64 does not guarantee cross-CPU TLB invalidation visibility
    // without an explicit barrier. SEQ_CST fence ensures all prior
    // PAGE_NOACCESS transitions are globally observable before we proceed
    // to unmap/remap the address range.
#ifdef LIBC_TARGET_ARCH_IS_AARCH64
    cpp::atomic_thread_fence(cpp::MemoryOrder::SEQ_CST);
#endif
  }

  // ── Step 5: Create placeholder at target ──
  PlaceholderRange new_ph;
  windows::MmapLockWriterGuard guard{
      windows::MmapLockWriterGuard::defer_acquire};

  if (fixed_addr) {
    if (int err = windows::alloc::pagemap::validate_map_fixed_target(fixed_addr, new_size)) {
      // Cannot clean up TLB shootdown here — fall through to restore below.
      // Restore original protections after TLB shootdown.
      if (rec_count > 0)
        windows::replay_protections(process, old_val, base_prot,
                                    records.data(), rec_count);
      else {
        PVOID pb = old_addr;
        SIZE_T ps = old_size;
        ULONG op;
        ::NtProtectVirtualMemory(process, &pb, &ps, base_prot, &op);
      }
      windows::g_mapping_table.abort_remap_guard(old_addr);
      register_view_slices(old_addr, slices, slice_count,
                           windows::memory::RegionPool::NONE, 0);
      release_view_slices(slices, slice_count);
      // ~ext_guard + ~fresh_guard close sections; cow_ctx RAII handles cleanup.
      return -err;
    }
    guard.acquire();
    if (!windows::prepare_for_fixed(fixed_addr, new_size)) {
      guard.release();
    } else if (windows::is_alloc_aligned(fixed_addr)) {
      new_ph = PlaceholderRange::from_raw(fixed_addr, new_size);
    } else {
      new_ph =
          PlaceholderRange::reserve_exact_with_retry(new_size, fixed_addr);
      if (!new_ph) {
        guard.release();
      }
    }
  } else {
    new_ph = PlaceholderRange::reserve(new_size);
  }

  if (!new_ph) {
    // Restore original protections after TLB shootdown.
    if (rec_count > 0)
      windows::replay_protections(process, old_val, base_prot,
                                  records.data(), rec_count);
    else {
      PVOID pb = old_addr;
      SIZE_T ps = old_size;
      ULONG op;
      ::NtProtectVirtualMemory(process, &pb, &ps, base_prot, &op);
    }
    windows::g_mapping_table.abort_remap_guard(old_addr);
    register_view_slices(old_addr, slices, slice_count,
                         windows::memory::RegionPool::NONE, 0);
    release_view_slices(slices, slice_count);
    // ~ext_guard + ~fresh_guard close sections; cow_ctx RAII handles cleanup.
    return -ENOMEM;
  }

  // ── Step 6: Map all view slices + extension at new address ──
  void *new_placeholder = new_ph.base();
  if (!map_slices_into_placeholder(process, new_placeholder, slices,
                                    slice_count, old_size, ext_section,
                                    delta, base_prot)) {
    // map_slices_into_placeholder may have partially split the placeholder.
    // Walk the range and release all fragments.
    // Disown from RAII — we handle the partially-split cleanup manually.
    (void)new_ph.consume();
    // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
    // which is what we want — fragment boundaries from the upfront capture
    // tell us where each placeholder ends, even as nt_pal::free_placeholder runs.
    auto ws = windows::byte_scratch(4096);
    if (ws) {
      nt_pal::RegionWalker walk(new_placeholder, new_size, ws.data(),
                                 ws.size());
      while (walk.next()) {
        nt_pal::free_placeholder(walk.entry->BaseAddress);
      }
    }
    // Scratch failure: cannot enumerate fragments — best-effort fall-through.

    guard.release();
    if (rec_count > 0)
      windows::replay_protections(process, old_val, base_prot,
                                  records.data(), rec_count);
    windows::g_mapping_table.abort_remap_guard(old_addr);
    register_view_slices(old_addr, slices, slice_count,
                         windows::memory::RegionPool::NONE, 0);
    release_view_slices(slices, slice_count);
    // ~ext_guard + ~fresh_guard close sections; cow_ctx RAII handles cleanup.
    return -ENOMEM;
  }

  // Mapping succeeded — placeholder consumed by the view slices.
  (void)new_ph.consume();

  guard.release();

  // ── Step 7: Unmap all old views ──
  {
    // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
    // which is what we want — the upfront capture lists every AllocationBase
    // to unmap, and nt_pal::unmap_view_preserve replaces views with placeholders
    // (kept for caller cleanup at Step 9).
    auto ws = windows::byte_scratch(4096);
    if (ws) {
      nt_pal::RegionWalker walk(old_addr, old_size, ws.data(), ws.size());
      while (walk.next()) {
        if (walk.entry->Type == MEM_MAPPED) {
          nt_pal::unmap_view_preserve(walk.entry->AllocationBase);
        }
      }
    }
    // Scratch failure: cannot enumerate views — best-effort fall-through.
  }

  // ── Step 8: Replay protections + restore COW at new address ──
  uintptr_t new_val = reinterpret_cast<uintptr_t>(new_placeholder);
  if (rec_count > 0)
    windows::replay_protections(process, new_val, base_prot,
                                records.data(), rec_count);
  cow_ctx.restore(new_val, records.data(), rec_count);

  // ── Step 9: Handle old_addr ──
  if (dontunmap) {
    // Map zero-filled section over the old range's placeholder(s).
    // Coalesce the old placeholder fragments first.
    nt_pal::coalesce_placeholders(old_addr, old_size);

    PVOID fresh_base = old_addr;
    SIZE_T fresh_size = old_size;
    LARGE_INTEGER fresh_offset = {};
    NTSTATUS map_st = ::NtMapViewOfSectionEx(
        fresh_section, process, &fresh_base, &fresh_offset, &fresh_size,
        MEM_REPLACE_PLACEHOLDER, base_prot, nullptr, 0);

    if (NT_ERROR(map_st)) {
      // Rollback: unmap new views, remap old slices at old_addr.
      char *undo = static_cast<char *>(new_placeholder);
      for (int i = 0; i < slice_count; ++i) {
        nt_pal::unmap_view(undo);
        undo += slices[i].size;
      }
      if (ext_section) {
        nt_pal::unmap_view(undo);
      }
      // Re-create placeholder at old_addr and map slices back.
      // (Best-effort — old_addr is already a placeholder from the unmap.)
      map_slices_into_placeholder(process, old_addr, slices, slice_count,
                                   old_size, nullptr, 0, 0);
      if (rec_count > 0)
        windows::replay_protections(process, old_val, base_prot,
                                    records.data(), rec_count);

      windows::g_mapping_table.abort_remap_guard(old_addr);
      register_view_slices(old_addr, slices, slice_count,
                           windows::memory::RegionPool::NONE, 0);
      release_view_slices(slices, slice_count);
      // ~ext_guard + ~fresh_guard close sections.
      return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(map_st));
    }

    // Acquire a region for the fresh DONTUNMAP section. Pagefile-backed,
    // anonymous from POSIX's perspective (no fd, no inode). Use the
    // ANON_RESERVE_SECTION shape — the universal anon-pagefile-section
    // descriptor — with NORESERVE set when the source mapping was lazily
    // committed. The flag drives demand-commit via VEH; cleared, the fresh
    // section is eagerly committed by SEC_COMMIT itself.
    {
      windows::memory::AcquireSpec fresh_spec{};
      fresh_spec.section_handle = fresh_section;
      fresh_spec.section_offset = fresh_offset;
      fresh_spec.shape = windows::memory::RegionShape::ANON_RESERVE_SECTION;
      fresh_spec.flags = fresh_noreserve
                             ? windows::memory::region_flag::NORESERVE
                             : uint16_t{0};
      fresh_spec.first_slot_key =
          reinterpret_cast<uintptr_t>(fresh_base) >> 16;
      fresh_spec.last_slot_key =
          fresh_spec.first_slot_key + (old_size >> 16);
      uint32_t fresh_region_id =
          windows::memory::g_region_pool.acquire_owned(fresh_spec);
      if (fresh_region_id == windows::memory::RegionPool::NONE) {
        // Pool OOM after fresh map succeeded — unmap to avoid leak.
        nt_pal::unmap_view(fresh_base);
        windows::g_mapping_table.abort_remap_guard(old_addr);
        release_view_slices(slices, slice_count);
        // fresh_guard still owns the section — ~fresh_guard closes it.
        return -ENOMEM;
      }
      // Pool now owns fresh_section.
      fresh_guard.dismiss();
      uint8_t fresh_alloc_id = windows::memory::g_region_pool
                                   .get_mutable(fresh_region_id)
                                   ->alloc_id;
      if (!windows::g_mapping_table.register_mapping(
              fresh_base, old_size, fresh_region_id, fresh_alloc_id,
              base_prot, /*flags=*/0)) {
        // Couldn't register the published view — release the region.
        nt_pal::unmap_view(fresh_base);
        windows::memory::g_region_pool.release(fresh_region_id);
        windows::g_mapping_table.abort_remap_guard(old_addr);
        release_view_slices(slices, slice_count);
        return -ENOMEM;
      }
    }
  } else {
    // Release all old placeholder fragments.
    // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
    // which is what we want — fragment boundaries from the upfront capture
    // tell us where each placeholder ends, even as nt_pal::free_placeholder runs.
    auto ws = windows::byte_scratch(4096);
    if (ws) {
      nt_pal::RegionWalker walk(old_addr, old_size, ws.data(), ws.size());
      while (walk.next()) {
        nt_pal::free_placeholder(walk.entry->AllocationBase);
      }
    }
    // Scratch failure: cannot enumerate fragments — best-effort fall-through.
  }

  // ── Step 10: Clear guard and register all views at new address ──
  windows::g_mapping_table.abort_remap_guard(old_addr);

  // Acquire an extension region (FILE_VIEW_RESERVE / NORESERVE) when growing
  // pagefile-backed. Caller passes ownership of ext_section to the pool;
  // the original ext_guard is dismissed regardless to keep close paths
  // single-source-of-truth.
  uint32_t ext_region_id = windows::memory::RegionPool::NONE;
  if (ext_section) {
    char *ext_base_addr = static_cast<char *>(new_placeholder) + old_size;
    ext_region_id = acquire_extension_region(ext_section, ext_base_addr,
                                             delta, base_prot);
    if (ext_region_id == windows::memory::RegionPool::NONE) {
      // Pool failed to take the section — unmap and bail.
      // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
      // which is what we want — the upfront capture lists every view to
      // unmap, and nt_pal::unmap_view replaces them with placeholders.
      auto ws = windows::byte_scratch(4096);
      if (ws) {
        nt_pal::RegionWalker walk(new_placeholder, new_size, ws.data(),
                                   ws.size());
        while (walk.next()) {
          if (walk.entry->Type == MEM_MAPPED)
            nt_pal::unmap_view(walk.entry->AllocationBase);
        }
      }
      // Scratch failure: cannot enumerate views — best-effort fall-through.
      release_view_slices(slices, slice_count);
      // ~ext_guard closes ext_section.
      return -ENOMEM;
    }
    ext_guard.dismiss(); // Pool now owns ext_section.
  }

  if (!register_view_slices(new_placeholder, slices, slice_count,
                            ext_region_id, base_prot, delta)) {
    // Registration failure (N4): unmap orphaned views to avoid silent leak.
    // Old mapping is already gone — this is a total failure under
    // extreme memory pressure (radix page allocation failure).
    // Bulk MBI via nt_pal::RegionWalker; snapshot reflects pre-mutation VA layout,
    // which is what we want — upfront capture lists every view to unmap.
    auto ws = windows::byte_scratch(4096);
    if (ws) {
      nt_pal::RegionWalker walk(new_placeholder, new_size, ws.data(),
                                 ws.size());
      while (walk.next()) {
        if (walk.entry->Type == MEM_MAPPED)
          nt_pal::unmap_view(walk.entry->AllocationBase);
      }
    }
    // Scratch failure: cannot enumerate views — best-effort fall-through.
    // register_view_slices already released remaining region refs on failure.
    return -ENOMEM;
  }
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(new_placeholder));
}

//==========================================================================
// Grow in-place (single-view coalesce-remap)
//==========================================================================
//
// Grow-in-place does NOT need the PAGE_NOACCESS TLB shootdown IPI.
// The Phase 1 readable-window problem from mremap-move does not exist
// here — base stays the same, so there is no stale-pointer scenario.
// The VEH remap guard alone is sufficient: threads faulting during the
// unmap→coalesce→remap window stall, then retry and succeed (same
// section, same base, data intact).
//
// Algorithm:
//   File-backed:
//     1. Reserve extension placeholder
//     2. Arm VEH guard, snapshot protections + COW
//     3. NtExtendSection (grows file + section, O(1))
//     4. Unmap → coalesce → map single extended view
//     5. Replay protections, restore COW, disarm guard
//     6. Register mapping
//
//   Pagefile-backed (pure adjacent-view, zero window):
//     1. Reserve extension placeholder
//     2. Create new zero section for extension (SEC_RESERVE)
//     3. Map extension section into extension placeholder
//     4. Register extension mapping
//     Old view is NEVER TOUCHED. No unmap, no remap, no VEH guard,
//     no TLB invalidation, no protection snapshot, no COW save/restore.
//     Two syscalls (NtCreateSectionEx + NtMapViewOfSectionEx). O(1).
//
// Precondition: adjacent VA at [base+old_size, base+new_size) must be
// MEM_FREE. If occupied, returns 0 → caller falls through to move.
//
// Returns address-as-long on success, 0 if in-place growth is not possible.

intptr_t grow_section_in_place(void *addr, SIZE_T old_size,
                                        SIZE_T new_size) {
  HANDLE process = NtCurrentProcess();
  SIZE_T delta = new_size - old_size;
  char *ext_addr = static_cast<char *>(addr) + old_size;

  // Quick check: is the extension range free?
  if (!nt_pal::is_range_free(ext_addr, delta))
    return 0;

  // Begin remap: extract entry and arm VEH guard atomically.
  windows::MappingEntry entry = {};
  if (!windows::g_mapping_table.begin_remap(addr, old_size, &entry))
    return 0;

  // Resolve the region descriptor. While the slot is REMAPPING, the slot
  // still holds the +1 ref so the region pointer is pinned.
  windows::memory::RegionDesc *region =
      windows::memory::g_region_pool.get_mutable(entry.region_id);
  if (region == nullptr || region->section_handle == nullptr) {
    windows::g_mapping_table.abort_remap(addr, entry);
    return 0;
  }
  HANDLE entry_section = region->section_handle;
  HANDLE entry_file = region->file_handle;
  LARGE_INTEGER entry_offset = region->section_offset;
  DWORD entry_prot = entry.view_prot;

  // Build a transient ViewSpec for `map_into` calls below.
  auto make_view_spec = [&]() {
    windows::ViewSpec s{};
    s.section = entry_section;
    s.file = entry_file;
    s.offset = entry_offset;
    s.prot = entry_prot;
    s.flags = entry.flags;
    return s;
  };

  // Acquire lock for the entire coalesce-remap sequence.
  windows::MmapLockWriterGuard guard;

  // Reserve extension placeholder at [base+old_size, base+new_size).
  PlaceholderRange ext_ph = PlaceholderRange::reserve(delta, ext_addr);
  if (!ext_ph || ext_ph.base() != ext_addr) {
    // ~ext_ph releases if reserve returned a different address.
    windows::g_mapping_table.abort_remap(addr, entry);
    return 0;
  }

  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);

  if (entry_file) {
    // ── File-backed: extend + coalesce + single remap ──
    auto records = internal::ScratchAlloc<RegionRecord>(MAX_REGION_RECORDS);
    if (!records) {
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }
    int rec_count = windows::snapshot_regions(addr_val, addr_val + old_size,
                                              records.data(),
                                              MAX_REGION_RECORDS);
    if (LIBC_UNLIKELY(rec_count < 0)) {
      // ~ext_ph releases the extension placeholder.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    // WSEX per-page COW scan + save dirty page content.
    windows::CowContext cow_ctx;
    if (!cow_ctx.prepare(addr_val, records.data(), rec_count)) {
      // ~ext_ph releases the extension placeholder.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    SIZE_T ext_max = static_cast<SIZE_T>(entry_offset.QuadPart) + new_size;
    NTSTATUS st = nt_helpers::extend_section(entry_section, ext_max);
    if (NT_ERROR(st)) {
      // cow_ctx RAII handles cleanup.
      // ~ext_ph releases the extension placeholder.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    // Unmap old view → placeholder.
    st = nt_pal::unmap_view_preserve_transient(addr);
    if (NT_ERROR(st)) {
      // cow_ctx RAII handles cleanup.
      // ~ext_ph releases the extension placeholder.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    // Coalesce into single placeholder.
    st = nt_pal::coalesce_placeholders(addr, new_size);
    if (NT_ERROR(st)) {
      // Recover: remap old view at original size.
      windows::ViewSpec spec = make_view_spec();
      spec.map_into(addr, old_size);
      if (rec_count > 0)
        windows::replay_protections(process, addr_val, entry_prot,
                                    records.data(), rec_count);
      cow_ctx.restore(addr_val, records.data(), rec_count);
      // ~ext_ph releases the extension placeholder.
      // View restored successfully — abort back to LIVE.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    // Coalesce succeeded — ext_ph's VA is now part of the coalesced
    // placeholder [addr, addr+new_size). Must consume to prevent the
    // destructor from releasing a merged address.
    (void)ext_ph.consume();

    // Map extended section as single view.
    {
      windows::ViewSpec spec = make_view_spec();
      st = spec.map_into(addr, new_size);
    }
    if (NT_ERROR(st)) {
      // Recover: split back, remap old view.
      nt_pal::split_placeholder(addr, old_size);
      {
        windows::ViewSpec spec = make_view_spec();
        spec.map_into(addr, old_size);
      }
      if (rec_count > 0)
        windows::replay_protections(process, addr_val, entry_prot,
                                    records.data(), rec_count);
      cow_ctx.restore(addr_val, records.data(), rec_count);
      nt_pal::free_placeholder(ext_addr);
      // View restored — abort back to LIVE.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    if (rec_count > 0)
      windows::replay_protections(process, addr_val, entry_prot,
                                  records.data(), rec_count);
    cow_ctx.restore(addr_val, records.data(), rec_count);

    guard.release();

    // Commit: REMAPPING -> LIVE with the extended view. The region_id is
    // unchanged (same section, same backing file); only the size grew.
    if (!windows::g_mapping_table.commit_remap(
            addr, addr, new_size, entry.region_id, entry.alloc_id,
            entry_prot, entry.flags)) {
      // commit_remap can only fail when the slot's REMAPPING state has
      // been mutated out from under us — under MmapLock writer that's a
      // protocol break, but we still must not leave NT holding the
      // extended view untracked. Tear the new view down so the VA
      // returns to MEM_FREE, then drop the slot's region ref via
      // discard_remap (REMAPPING → FREE). The original view was unmapped
      // earlier in this branch, so there is nothing to restore — caller
      // sees -ENOMEM (mremap may move semantics: the original mapping is
      // already gone from the caller's perspective).
      nt_pal::unmap_view_preserve(addr);
      nt_pal::free_placeholder(addr);
      windows::g_mapping_table.discard_remap(addr);
      return 0;
    }
    return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));

  } else {
    // ── Pagefile-backed: pure adjacent-view ──
    // The old view is NEVER unmapped. Only the extension placeholder gets
    // a new section. Zero window — no VEH guard needed, no protection
    // snapshot/replay, no COW save/restore, no TLB invalidation.
    //   [base, base+old_size)         → untouched (old view stays live)
    //   [base+old_size, base+new_size) → new zero section (SEC_RESERVE)
    //
    // All downstream ops (munmap, mprotect, madvise) walk by address
    // and handle heterogeneous views naturally.

    // Create zero section for the extension.
    LARGE_INTEGER ext_sec_size;
    ext_sec_size.QuadPart = static_cast<LONGLONG>(delta);
    HANDLE ext_section = nullptr;
    auto ext_oa2 = windows::internal_oa();
    NTSTATUS st = ::NtCreateSectionEx(
        &ext_section,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
            SECTION_QUERY,
        &ext_oa2, &ext_sec_size, PAGE_EXECUTE_READWRITE, SEC_RESERVE,
        nullptr, nullptr, 0);
    if (NT_ERROR(st)) {
      // ~ext_ph releases the extension placeholder.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    // Map extension section into the extension placeholder.
    {
      windows::ViewSpec ext_spec{};
      ext_spec.section = ext_section;
      ext_spec.offset = LARGE_INTEGER{};
      ext_spec.prot = entry_prot;
      ext_spec.flags = 0;
      st = ext_spec.map_into(ext_addr, delta);
    }
    if (NT_ERROR(st)) {
      ::NtClose(ext_section);
      // ~ext_ph releases the extension placeholder.
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }

    // map_into succeeded — placeholder consumed by the view.
    (void)ext_ph.consume();

    guard.release();

    // Acquire a region for the extension and register it as a separate
    // slot. Shape is ANON_RESERVE_SECTION — pagefile-backed SEC_RESERVE
    // section with no fd, demand-committed by VEH on first access.
    uint32_t ext_region_id =
        acquire_extension_region(ext_section, ext_addr, delta, entry_prot);
    if (ext_region_id == windows::memory::RegionPool::NONE) {
      // Pool OOM — undo the view and roll back.
      nt_pal::unmap_view_preserve(ext_addr);
      ::NtClose(ext_section);
      nt_pal::free_placeholder(ext_addr);
      windows::g_mapping_table.abort_remap(addr, entry);
      return 0;
    }
    uint8_t ext_alloc_id = windows::memory::g_region_pool
                               .get_mutable(ext_region_id)
                               ->alloc_id.load(cpp::MemoryOrder::RELAXED);

    // Restore original entry (old view was never unmapped) and register
    // the extension as a separate entry. abort_remap puts the entry back
    // into its slot before we publish the extension.
    windows::g_mapping_table.abort_remap(addr, entry);
    if (!windows::g_mapping_table.register_mapping(
            ext_addr, delta, ext_region_id, ext_alloc_id, entry_prot,
            /*flags=*/0)) {
      nt_pal::unmap_view_preserve(ext_addr);
      windows::memory::g_region_pool.release(ext_region_id);
      nt_pal::free_placeholder(ext_addr);
      return 0;
    }
    return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
  }
}

/// Grow a bare placeholder in-place by coalescing with adjacent free VA.
/// Returns address-as-long on success, 0 if not possible.
intptr_t grow_bare_placeholder_in_place(void *addr, SIZE_T old_size,
                                                 SIZE_T new_size) {
  SIZE_T delta = new_size - old_size;
  char *ext_addr = static_cast<char *>(addr) + old_size;

  if (!nt_pal::is_range_free(ext_addr, delta))
    return 0;

  PlaceholderRange ext = PlaceholderRange::reserve(delta, ext_addr);
  if (!ext || ext.base() != ext_addr) {
    // ~ext releases if reserve returned a different address.
    return 0;
  }

  // Coalesce into single placeholder [base, base+new_size).
  NTSTATUS st = nt_pal::coalesce_placeholders(addr, new_size);
  if (NT_ERROR(st)) {
    // ~ext releases the extension placeholder.
    return 0;
  }

  // Coalesce succeeded — ext's VA merged into the coalesced placeholder.
  (void)ext.consume();
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
}

//==========================================================================
// Bare placeholder operations (PROT_NONE mmap, not yet demand-mapped)
//==========================================================================

/// Shrink a bare placeholder by splitting and releasing the tail.
/// Returns address-as-long on success, -errno on failure.
intptr_t shrink_bare_placeholder(void *addr, SIZE_T new_size) {
  if (!nt_pal::split_placeholder(addr, new_size)) {
    return -ENOMEM;
  }

  char *tail = static_cast<char *>(addr) + new_size;
  nt_pal::free_placeholder(tail);
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
}

/// Move a bare placeholder to a new address. No data to preserve --
/// bare placeholders are PROT_NONE with no committed content.
/// Returns address-as-long on success, -errno on failure.
intptr_t move_bare_placeholder(void *old_addr, SIZE_T new_size,
                                        void *fixed_addr,
                                        bool dontunmap) {
  PlaceholderRange new_ph;
  if (fixed_addr) {
    if (int err = windows::alloc::pagemap::validate_map_fixed_target(fixed_addr, new_size)) {
      return -err;
    }
    {
      windows::MmapLockWriterGuard guard;
      if (!windows::prepare_for_fixed(fixed_addr, new_size))
        return -ENOMEM;
      if (windows::is_alloc_aligned(fixed_addr)) {
        new_ph = PlaceholderRange::from_raw(fixed_addr, new_size);
      } else {
        new_ph =
            PlaceholderRange::reserve_exact_with_retry(new_size, fixed_addr);
      }
    }

    if (!new_ph) {
      return -ENOMEM;
    }
  } else {
    new_ph = PlaceholderRange::reserve(new_size);
    if (!new_ph) {
      return -ENOMEM;
    }
  }

  if (!dontunmap)
    nt_pal::free_placeholder(old_addr);
  // dontunmap: old placeholder stays as-is (already inaccessible).

  // Bare placeholder stays reserved — consume to transfer ownership to caller.
  auto result = new_ph.consume();
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(result.base));
}

//==========================================================================
// Private committed memory operations (MAP_ANONYMOUS without MAP_NORESERVE)
//==========================================================================

/// Grow private committed memory in-place by reserving + committing adjacent
/// placeholder VA. Two syscalls (placeholder reserve + commit) when VA is
/// free and 64KB-aligned. Returns address-as-long on success, 0 if not
/// possible (VA occupied or alignment mismatch).
intptr_t grow_private_in_place(void *addr, SIZE_T old_size,
                                        SIZE_T new_size) {
  SIZE_T delta = new_size - old_size;
  char *ext_addr = static_cast<char *>(addr) + old_size;

  PlaceholderRange ext = PlaceholderRange::reserve(delta, ext_addr);
  if (!ext || ext.base() != ext_addr)
    return 0; // VA occupied or kernel chose differently — ~ext releases.

  NTSTATUS st = ext.commit(PAGE_READWRITE);
  if (NT_ERROR(st))
    return 0; // ~ext releases the placeholder.

  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
}

/// Shrink private committed memory by releasing the tail to MEM_FREE.
/// Single syscall via nt_pal::interior_release. Returns address-as-long on
/// success, -errno on failure.
intptr_t shrink_private(void *addr, SIZE_T old_size,
                                 SIZE_T new_size) {
  char *tail = static_cast<char *>(addr) + new_size;
  SIZE_T tail_size = old_size - new_size;

  // Interior release: tail becomes MEM_FREE in one syscall.
  // Replaces the three-step decommit+preserve+release sequence.
  if (!nt_pal::interior_release(tail, tail_size))
    return -ENOMEM;
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
}

/// Move private committed memory to a new address via memcpy.
/// No zero-copy — private memory can't remap sections.
///
/// Placeholder reserve + commit for both fixed and non-fixed targets.
/// Fixed: prepare_for_fixed tears down; placeholder adopted or created
/// at the exact address. Non-fixed: system-chosen placeholder.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t move_private(void *old_addr, SIZE_T old_size,
                               SIZE_T new_size, void *fixed_addr,
                               bool dontunmap) {
  PlaceholderRange new_ph;

  if (fixed_addr) {
    // MREMAP_FIXED: tear down target VA and allocate there.
    if (int err = windows::alloc::pagemap::validate_map_fixed_target(fixed_addr, new_size))
      return -err;

    {
      windows::MmapLockWriterGuard guard;
      if (!windows::prepare_for_fixed(fixed_addr, new_size))
        return -ENOMEM;
    }

    if (windows::is_alloc_aligned(fixed_addr)) {
      new_ph = PlaceholderRange::from_raw(fixed_addr, new_size);
    } else {
      new_ph =
          PlaceholderRange::reserve_exact_with_retry(new_size, fixed_addr);
    }
    if (!new_ph)
      return -ENOMEM;
  } else {
    new_ph = PlaceholderRange::reserve(new_size);
    if (!new_ph)
      return -ENOMEM;
  }

  void *new_base = new_ph.base();
  NTSTATUS st = new_ph.commit(PAGE_READWRITE);
  if (NT_ERROR(st))
    return -ENOMEM; // ~new_ph releases placeholder.

  // Copy old data.
  SIZE_T copy_size = (old_size < new_size) ? old_size : new_size;
  __builtin_memcpy(new_base, old_addr, copy_size);

  // Free or preserve old allocation.
  if (!dontunmap)
    nt_pal::free_va(old_addr);

  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(new_base));
}

} // namespace

//==========================================================================
// Engine entry point
//==========================================================================
//
// Returns address-as-long on success, -errno on failure.
// Never sets libc_errno.

intptr_t internal::mremap(void *old_address, size_t old_size, size_t new_size,
                          int flags, void *new_address) {
  if (LIBC_UNLIKELY(!old_address || old_size == 0 || new_size == 0)) {
    return -EINVAL;
  }

  if (LIBC_UNLIKELY(!windows::is_page_aligned(old_address))) {
    return -EINVAL;
  }

  if (LIBC_UNLIKELY((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))) {
    return -EINVAL;
  }

  if (LIBC_UNLIKELY((flags & MREMAP_DONTUNMAP) && old_size != new_size)) {
    return -EINVAL;
  }

  if (LIBC_UNLIKELY((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))) {
    return -EINVAL;
  }

  if ((flags & MREMAP_FIXED) &&
      LIBC_UNLIKELY(!new_address || !windows::is_page_aligned(new_address))) {
    return -EINVAL;
  }

  if (LIBC_UNLIKELY((flags &
                     ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP)) !=
                    0)) {
    return -EINVAL;
  }

  // Round to page boundary. Placeholder splits are page-granular.
  const SIZE_T gran_old = windows::round_to_page(old_size);
  const SIZE_T gran_new = windows::round_to_page(new_size);
  if (LIBC_UNLIKELY(gran_old == 0 || gran_new == 0)) {
    return -ENOMEM;
  }

  // Determine region type. Shape-driven path: snapshot the slot covering
  // old_address and dispatch by RegionShape. The snapshot resolves the
  // region descriptor under the seqlock fence with alloc_id validation;
  // a null `region` (no slot or pool reuse) falls back to the legacy MBI
  // classifier so untracked allocations still work.
  bool is_section = false;
  bool is_bare_placeholder = false;
  bool is_private_committed = false;
  bool snap_is_anon = false;

  char *aligned_old = reinterpret_cast<char *>(
      windows::align_down_to_granularity(
          reinterpret_cast<uintptr_t>(old_address)));
  windows::SlotSnapshot snap;
  bool have_snap =
      windows::g_mapping_table.snapshot(aligned_old, &snap) &&
      snap.region != nullptr;

  if (have_snap) {
    const windows::memory::RegionShape shape = snap.region->current_shape();
    switch (shape) {
    case windows::memory::RegionShape::ANON_PLACEHOLDER:
      // COMMITTED flag: fully-committed MEM_PRIVATE (kernel-identical to
      // the retired ANON_ONESHOT) — grow-in-place and direct memcpy work.
      // PROT_NONE / NORESERVE: not fully committed — treat as bare
      // placeholder so move/grow paths don't assume committed pages.
      if (snap.region->has_flag(windows::memory::region_flag::COMMITTED))
        is_private_committed = true;
      else
        is_bare_placeholder = true;
      snap_is_anon = true;
      break;
    case windows::memory::RegionShape::FILE_VIEW_MONO:
    case windows::memory::RegionShape::FILE_VIEW_CHUNKED:
    case windows::memory::RegionShape::FILE_VIEW_RESERVE:
    case windows::memory::RegionShape::ANON_RESERVE_SECTION:
      is_section = true;
      // ANON_RESERVE_SECTION has no fd backing — treat as anonymous for
      // MREMAP_DONTUNMAP eligibility below.
      snap_is_anon =
          (shape == windows::memory::RegionShape::ANON_RESERVE_SECTION);
      break;
    case windows::memory::RegionShape::LIBC_INTERNAL:
    case windows::memory::RegionShape::IMAGE_REGION:
    case windows::memory::RegionShape::KERNEL_REGION:
      return -EINVAL;
    case windows::memory::RegionShape::FOREIGN_SENTINEL:
      return -EINVAL;
    case windows::memory::RegionShape::NONE:
      have_snap = false;
      break;
    }
  }

  if (!have_snap) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!nt_pal::query_region(old_address, mbi) || mbi.State == MEM_FREE)
      return -EINVAL;
    if (mbi.Type == MEM_IMAGE)
      return -EINVAL;
    is_section = (mbi.Type == MEM_MAPPED);
    is_bare_placeholder =
        (mbi.Type == MEM_PRIVATE && mbi.State == MEM_RESERVE);
    is_private_committed =
        (mbi.Type == MEM_PRIVATE && mbi.State == MEM_COMMIT);
    if (!is_section && !is_bare_placeholder && !is_private_committed)
      return -EINVAL;
  }

  const bool dontunmap = (flags & MREMAP_DONTUNMAP);

  // MREMAP_DONTUNMAP: only valid for anonymous mappings. Shape lookup
  // already answered this via snap_is_anon for tracked regions; the
  // table-miss fallback re-asks via snapshot for legacy file views.
  if (dontunmap && is_section) {
    if (have_snap) {
      if (!snap_is_anon)
        return -EINVAL;
    } else {
      windows::SlotSnapshot s2;
      if (!windows::g_mapping_table.snapshot(old_address, &s2))
        return -EINVAL;
      if (s2.region != nullptr && s2.region->file_handle != nullptr)
        return -EINVAL;
    }
  }

  // ── Same size (after granularity rounding) ──
  if (gran_new == gran_old) {
    if (flags & MREMAP_FIXED) {
      if (is_private_committed)
        return move_private(old_address, gran_old, gran_new, new_address,
                            dontunmap);
      if (is_section)
        return move_section(old_address, gran_old, gran_new, new_address,
                            dontunmap);
      return move_bare_placeholder(old_address, gran_new, new_address,
                                   dontunmap);
    }
    if (dontunmap) {
      if (is_private_committed)
        return move_private(old_address, gran_old, gran_new, nullptr, true);
      if (is_section)
        return move_section(old_address, gran_old, gran_new, nullptr, true);
      return move_bare_placeholder(old_address, gran_new, nullptr, true);
    }
    return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(old_address));
  }

  // ── Shrink ──
  if (gran_new < gran_old) {
    intptr_t shrunk;
    if (is_private_committed)
      shrunk = shrink_private(old_address, gran_old, gran_new);
    else if (is_section)
      shrunk = shrink_section_view(old_address, gran_old, gran_new);
    else
      shrunk = shrink_bare_placeholder(old_address, gran_new);
    if (shrunk < 0)
      return shrunk;

    void *shrunk_addr = reinterpret_cast<void *>(static_cast<uintptr_t>(shrunk));

    if (flags & MREMAP_FIXED) {
      if (is_private_committed)
        return move_private(shrunk_addr, gran_new, gran_new, new_address,
                            dontunmap);
      if (is_section)
        return move_section(shrunk_addr, gran_new, gran_new, new_address,
                            dontunmap);
      return move_bare_placeholder(shrunk_addr, gran_new, new_address,
                                   dontunmap);
    }
    return shrunk;
  }

  // ── Grow ──

  // Try in-place growth first.
  if (!(flags & MREMAP_FIXED) && !dontunmap) {
    intptr_t result = 0;
    if (is_private_committed)
      result = grow_private_in_place(old_address, gran_old, gran_new);
    else if (is_section)
      result = grow_section_in_place(old_address, gran_old, gran_new);
    else
      result = grow_bare_placeholder_in_place(old_address, gran_old, gran_new);
    if (result)
      return result;
  }

  if (!(flags & MREMAP_MAYMOVE)) {
    return -ENOMEM;
  }

  // Grow with move.
  void *move_fixed = (flags & MREMAP_FIXED) ? new_address : nullptr;
  if (is_private_committed)
    return move_private(old_address, gran_old, gran_new, move_fixed, dontunmap);
  if (is_section)
    return move_section(old_address, gran_old, gran_new, move_fixed, dontunmap);
  return move_bare_placeholder(old_address, gran_new, move_fixed, dontunmap);
}

} // namespace LIBC_NAMESPACE_DECL
