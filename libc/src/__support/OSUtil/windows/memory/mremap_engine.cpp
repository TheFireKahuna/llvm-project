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
//   Pagefile-backed: NtExtendSection does not work (STATUS_SECTION_TOO_BIG).
//     A new section is created at the target size, data is copied via a
//     temporary view, and the new section replaces the old one.
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
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/remap_transaction.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/memory_region.h"
#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/region_snapshot.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

//==========================================================================
// Utilities
//==========================================================================

/// Close handles from an extracted mapping entry (null-safe).
void close_entry_handles(windows::MappingEntry &entry) {
  if (entry.spec.file)
    ::NtClose(entry.spec.file);
  if (entry.spec.section)
    ::NtClose(entry.spec.section);
}

//==========================================================================
// Multi-view collection for copyless mremap move
//==========================================================================
//
// After grow-in-place, a logical mapping may span multiple adjacent section
// views (e.g., original + extension). move_section must relocate ALL views,
// not just the first. Collect them upfront by walking the address range.

/// A single section view within a multi-view logical mapping.
struct ViewSliceEntry {
  windows::ViewSpec spec;
  SIZE_T size;             // Size of this view
};

constexpr int MAX_VIEW_SLICES = 16;

/// Walk [addr, addr+total_size) and extract all mapping table entries for
/// MEM_MAPPED views. Returns the number of slices collected. On failure,
/// closes any already-extracted handles and returns 0.
int collect_view_slices(void *addr, SIZE_T total_size,
                                     ViewSliceEntry *out,
                                     int max_slices) {
  char *cur = static_cast<char *>(addr);
  char *end = cur + total_size;
  int count = 0;

  while (cur < end && count < max_slices) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!windows::query_region(cur, mbi))
      break;

    char *region_end = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    if (region_end > end)
      region_end = end;

    if (mbi.State != MEM_FREE && mbi.Type == MEM_MAPPED &&
        cur == mbi.AllocationBase) {
      // This is a view base — extract its mapping table entry.
      windows::MappingEntry entry = {};
      if (windows::g_mapping_table.extract(cur, &entry) &&
          entry.spec.section) {
        // Find the full view size (may span multiple MBI regions).
        char *view_end = windows::find_alloc_end(cur);
        if (view_end > end)
          view_end = end;

        out[count].spec = entry.spec;
        // Adjust section offset when cur differs from the entry's view_base.
        // After grow-in-place, a view may be collected starting partway into
        // the section — the offset must advance by the distance from the
        // original base to prevent reading wrong file content on remap.
        out[count].spec.offset.QuadPart +=
            static_cast<LONGLONG>(
                cur - static_cast<char *>(entry.view_base));
        out[count].size = static_cast<SIZE_T>(view_end - cur);
        ++count;
        cur = view_end;
        continue;
      }
      close_entry_handles(entry);
    }

    cur = region_end;
  }

  return count;
}

/// Release all handles in a collected slice array.
void close_view_slices(ViewSliceEntry *slices, int count) {
  for (int i = 0; i < count; ++i) {
    if (slices[i].spec.section)
      ::NtClose(slices[i].spec.section);
    if (slices[i].spec.file)
      ::NtClose(slices[i].spec.file);
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
    if (!windows::split_placeholder(split_base, slices[i].size))
      return false;
    split_base += slices[i].size;
  }

  // Split before extension if present.
  if (extra_section && total_old_size < total) {
    if (!windows::split_placeholder(split_base, slices[slice_count - 1].size))
      return false;
  }

  // Map each slice.
  int mapped = 0;
  for (int i = 0; i < slice_count; ++i) {
    NTSTATUS st = slices[i].spec.map_into(base, slices[i].size);
    if (NT_ERROR(st)) {
      // Unmap already-mapped slices.
      char *undo = static_cast<char *>(target);
      for (int j = 0; j < mapped; ++j) {
        ::NtUnmapViewOfSectionEx(process, undo, 0);
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
        ::NtUnmapViewOfSectionEx(process, undo, 0);
        undo += slices[j].size;
      }
      return false;
    }
  }

  return true;
}

/// Register all slices + optional extension in the mapping table.
/// Takes ownership of all handles: slice handles are nulled, and
/// *extra_section is nulled if non-null. Callers must not close
/// these handles after this call.
bool register_view_slices(void *target, ViewSliceEntry *slices,
                                       int count, HANDLE *extra_section,
                                       DWORD extra_prot,
                                       SIZE_T extra_size = 0) {
  char *base = static_cast<char *>(target);
  for (int i = 0; i < count; ++i) {
    bool ok = windows::g_mapping_table.register_mapping_take(
        base, slices[i].size, slices[i].spec);
    slices[i].spec.file = nullptr;    // Ownership transferred regardless.
    slices[i].spec.section = nullptr;
    if (!ok)
      return false;
    base += slices[i].size;
  }
  if (extra_section && *extra_section) {
    LARGE_INTEGER ext_off = {};
    if (!windows::g_mapping_table.register_mapping_take(
            base, extra_size,
            windows::ViewSpec{*extra_section, /*file=*/nullptr, ext_off,
                              extra_prot, windows::VM_FLAG_SEC_RESERVE})) {
      *extra_section = nullptr;
      return false;
    }
    *extra_section = nullptr;
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

  if (!txn.entry().spec.section) {
    // ~txn handles cleanup.
    return -EINVAL;
  }

  auto result = txn.execute();
  if (!result.target)
    return -ENOMEM; // ~txn rolls back.

  // Release the tail placeholder (freeing the VA range).
  // consume() transfers ownership out; release_placeholder() frees the VA.
  auto [tail_base, tail_size] = result.target.consume();
  windows::release_placeholder(tail_base);

  if (!result.left_ok) {
    // Prefix remap failed — can't shrink.
    return -ENOMEM; // ~txn rolls back remaining state.
  }

  txn.commit();
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
  windows::g_mapping_table.begin_remap_guard(old_addr, old_size);

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
  DWORD base_prot = slices[0].spec.prot;
  // ── Step 1: Pre-create sections for grow + DONTUNMAP (fail-fast) ──
  HANDLE ext_section = nullptr;
  HANDLE fresh_section = nullptr;
  SIZE_T delta = (new_size > old_size) ? new_size - old_size : 0;

  if (delta > 0) {
    // Always use adjacent-view for pagefile grow. For file-backed single
    // view, NtExtendSection would have already produced a single larger
    // view (handled by grow_section_in_place). If we're in move_section
    // with a single file-backed view that needs growing, extend it.
    if (slice_count == 1) {
      // Try extend (works for file-backed, fails for pagefile).
      SIZE_T ext_max = static_cast<SIZE_T>(slices[0].spec.offset.QuadPart) +
                       new_size;
      NTSTATUS ext_st = nt_helpers::extend_section(slices[0].spec.section,
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
        close_view_slices(slices, slice_count);
        return -ENOMEM;
      }
    }
  }

  if (dontunmap) {
    LARGE_INTEGER sec_size;
    sec_size.QuadPart = static_cast<LONGLONG>(old_size);
    auto fresh_oa = windows::internal_oa();
    NTSTATUS st = ::NtCreateSectionEx(
        &fresh_section,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
            SECTION_QUERY,
        &fresh_oa, &sec_size, PAGE_EXECUTE_READWRITE, SEC_COMMIT, nullptr,
        nullptr, 0);
    if (NT_ERROR(st)) {
      windows::g_mapping_table.abort_remap_guard(old_addr);
      if (ext_section)
        ::NtClose(ext_section);
      close_view_slices(slices, slice_count);
      return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(st));
    }
  }

  uintptr_t old_val = reinterpret_cast<uintptr_t>(old_addr);

  // ── Step 3: Unified snapshot + COW writeback ──
  RegionRecord records[MAX_REGION_RECORDS];
  int rec_count = windows::snapshot_regions(old_val, old_val + old_size,
                                            records,
                                            MAX_REGION_RECORDS);
  if (LIBC_UNLIKELY(rec_count < 0)) {
    windows::g_mapping_table.abort_remap_guard(old_addr);
    if (ext_section)
      ::NtClose(ext_section);
    if (fresh_section)
      ::NtClose(fresh_section);
    register_view_slices(old_addr, slices, slice_count, nullptr, 0);
    close_view_slices(slices, slice_count);
    return -ENOMEM;
  }

  // WSEX per-page COW scan + save dirty page content.
  windows::CowContext cow_ctx;
  if (!cow_ctx.prepare(old_val, records, rec_count)) {
    windows::g_mapping_table.abort_remap_guard(old_addr);
    if (ext_section)
      ::NtClose(ext_section);
    if (fresh_section)
      ::NtClose(fresh_section);
    register_view_slices(old_addr, slices, slice_count, nullptr, 0);
    close_view_slices(slices, slice_count);
    return -ENOMEM;
  }

  // ── Step 4: TLB shootdown — PAGE_NOACCESS on all views ──
  // NtProtectVirtualMemory operates per-allocation-region. For multi-view
  // mappings, protect each view separately. The cross-CPU IPI on each call
  // flushes TLB entries for that range.
  {
    char *prot_cur = static_cast<char *>(old_addr);
    char *prot_end = prot_cur + old_size;
    while (prot_cur < prot_end) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!windows::query_region(prot_cur, mbi))
        break;
      if (mbi.State == MEM_COMMIT) {
        PVOID pb = prot_cur;
        SIZE_T ps = static_cast<SIZE_T>(
            (static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize < prot_end)
                ? (static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize -
                   prot_cur)
                : (prot_end - prot_cur));
        ULONG old_prot;
        ::NtProtectVirtualMemory(process, &pb, &ps, PAGE_NOACCESS, &old_prot);
      }
      char *next = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= prot_cur)
        break;
      prot_cur = next;
    }

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
  bool holding_lock = false;

  if (fixed_addr) {
    if (int err = windows::validate_map_fixed_target(fixed_addr, new_size)) {
      // Cannot clean up TLB shootdown here — fall through to restore below.
      // Restore original protections after TLB shootdown.
      if (rec_count > 0)
        windows::replay_protections(process, old_val, base_prot,
                                    records, rec_count);
      else {
        PVOID pb = old_addr;
        SIZE_T ps = old_size;
        ULONG op;
        ::NtProtectVirtualMemory(process, &pb, &ps, base_prot, &op);
      }
      windows::g_mapping_table.abort_remap_guard(old_addr);
      if (ext_section)
        ::NtClose(ext_section);
      if (fresh_section)
        ::NtClose(fresh_section);
      register_view_slices(old_addr, slices, slice_count, nullptr, 0);
      close_view_slices(slices, slice_count);
      // cow_ctx RAII handles cleanup.
      return -err;
    }
    windows::g_mmap_lock.acquire_exclusive();
    holding_lock = true;
    if (!windows::prepare_for_fixed(fixed_addr, new_size)) {
      windows::g_mmap_lock.release_exclusive();
      holding_lock = false;
    } else if (windows::is_alloc_aligned(fixed_addr)) {
      new_ph = PlaceholderRange::from_raw(fixed_addr, new_size);
    } else {
      new_ph =
          PlaceholderRange::reserve_exact_with_retry(new_size, fixed_addr);
      if (!new_ph) {
        windows::g_mmap_lock.release_exclusive();
        holding_lock = false;
      }
    }
  } else {
    new_ph = PlaceholderRange::reserve(new_size);
  }

  if (!new_ph) {
    // Restore original protections after TLB shootdown.
    if (rec_count > 0)
      windows::replay_protections(process, old_val, base_prot,
                                  records, rec_count);
    else {
      PVOID pb = old_addr;
      SIZE_T ps = old_size;
      ULONG op;
      ::NtProtectVirtualMemory(process, &pb, &ps, base_prot, &op);
    }
    windows::g_mapping_table.abort_remap_guard(old_addr);
    if (ext_section)
      ::NtClose(ext_section);
    if (fresh_section)
      ::NtClose(fresh_section);
    register_view_slices(old_addr, slices, slice_count, nullptr, 0);
    close_view_slices(slices, slice_count);
    // cow_ctx RAII handles cleanup.
    return -ENOMEM;
  }

  // ── Step 6: Map all view slices + extension at new address ──
  void *new_placeholder = new_ph.base();
  if (!map_slices_into_placeholder(process, new_placeholder, slices,
                                    slice_count, old_size, ext_section,
                                    delta, base_prot)) {
    // map_slices_into_placeholder may have partially split the placeholder.
    // Walk the range and release all fragments.
    char *ph = static_cast<char *>(new_placeholder);
    char *ph_end = ph + new_size;
    // Disown from RAII — we handle the partially-split cleanup manually.
    (void)new_ph.consume();
    while (ph < ph_end) {
      PVOID b = ph;
      SIZE_T s = 0;
      ::NtFreeVirtualMemory(process, &b, &s, MEM_RELEASE);
      MEMORY_BASIC_INFORMATION mbi;
      if (windows::query_region(ph, mbi))
        ph = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      else
        break;
    }

    if (holding_lock)
      windows::g_mmap_lock.release_exclusive();
    if (rec_count > 0)
      windows::replay_protections(process, old_val, base_prot,
                                  records, rec_count);
    windows::g_mapping_table.abort_remap_guard(old_addr);
    if (ext_section)
      ::NtClose(ext_section);
    if (fresh_section)
      ::NtClose(fresh_section);
    register_view_slices(old_addr, slices, slice_count, nullptr, 0);
    close_view_slices(slices, slice_count);
    // cow_ctx RAII handles cleanup.
    return -ENOMEM;
  }

  // Mapping succeeded — placeholder consumed by the view slices.
  (void)new_ph.consume();

  if (holding_lock) {
    windows::g_mmap_lock.release_exclusive();
    holding_lock = false;
  }

  // ── Step 7: Unmap all old views ──
  {
    char *cur = static_cast<char *>(old_addr);
    char *end = cur + old_size;
    while (cur < end) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!windows::query_region(cur, mbi))
        break;
      if (mbi.Type == MEM_MAPPED) {
        ::NtUnmapViewOfSectionEx(process, mbi.AllocationBase,
                                  MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
      }
      char *region_end = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (region_end <= cur)
        break;
      cur = region_end;
    }
  }

  // ── Step 8: Replay protections + restore COW at new address ──
  uintptr_t new_val = reinterpret_cast<uintptr_t>(new_placeholder);
  if (rec_count > 0)
    windows::replay_protections(process, new_val, base_prot,
                                records, rec_count);
  cow_ctx.restore(new_val, records, rec_count);

  // ── Step 9: Handle old_addr ──
  if (dontunmap) {
    // Map zero-filled section over the old range's placeholder(s).
    // Coalesce the old placeholder fragments first.
    PVOID coal_base = old_addr;
    SIZE_T coal_size = old_size;
    ::NtFreeVirtualMemory(process, &coal_base, &coal_size,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);

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
        ::NtUnmapViewOfSectionEx(process, undo, 0);
        undo += slices[i].size;
      }
      if (ext_section) {
        ::NtUnmapViewOfSectionEx(process, undo, 0);
      }
      // Re-create placeholder at old_addr and map slices back.
      // (Best-effort — old_addr is already a placeholder from the unmap.)
      map_slices_into_placeholder(process, old_addr, slices, slice_count,
                                   old_size, nullptr, 0, 0);
      if (rec_count > 0)
        windows::replay_protections(process, old_val, base_prot,
                                    records, rec_count);

      ::NtClose(fresh_section);
      windows::g_mapping_table.abort_remap_guard(old_addr);
      register_view_slices(old_addr, slices, slice_count, nullptr, 0);
      close_view_slices(slices, slice_count);
      if (ext_section)
        ::NtClose(ext_section);
      return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(map_st));
    }

    windows::g_mapping_table.register_mapping_take(
        fresh_base, old_size,
        windows::ViewSpec{fresh_section, /*file=*/nullptr, fresh_offset,
                          base_prot, /*flags=*/0});
  } else {
    // Release all old placeholder fragments.
    char *cur = static_cast<char *>(old_addr);
    char *end = cur + old_size;
    while (cur < end) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!windows::query_region(cur, mbi))
        break;
      PVOID b = mbi.AllocationBase;
      SIZE_T s = 0;
      ::NtFreeVirtualMemory(process, &b, &s, MEM_RELEASE);
      char *region_end = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (region_end <= cur)
        break;
      cur = region_end;
    }
  }

  // ── Step 10: Clear guard and register all views at new address ──
  windows::g_mapping_table.abort_remap_guard(old_addr);

  if (!register_view_slices(new_placeholder, slices, slice_count, &ext_section,
                            base_prot, delta)) {
    // Registration failure (N4): unmap orphaned views to avoid silent leak.
    // Old mapping is already gone — this is a total failure under
    // extreme memory pressure (radix page allocation failure).
    char *undo = static_cast<char *>(new_placeholder);
    char *undo_end = undo + new_size;
    while (undo < undo_end) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!windows::query_region(undo, mbi))
        break;
      if (mbi.Type == MEM_MAPPED)
        ::NtUnmapViewOfSectionEx(process, mbi.AllocationBase, 0);
      char *next = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= undo)
        break;
      undo = next;
    }
    close_view_slices(slices, slice_count);
    if (ext_section)
      ::NtClose(ext_section);
    return -ENOMEM;
  }
  close_view_slices(slices, slice_count);
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
  if (!windows::is_range_free(ext_addr, delta))
    return 0;

  // Begin remap: extract entry and arm VEH guard atomically.
  windows::MappingEntry entry = {};
  if (!windows::g_mapping_table.begin_remap(addr, old_size, &entry))
    return 0;
  if (!entry.spec.section) {
    windows::g_mapping_table.discard_remap(addr);
    return 0;
  }

  // Acquire lock for the entire coalesce-remap sequence.
  windows::g_mmap_lock.acquire_exclusive();

  // Reserve extension placeholder at [base+old_size, base+new_size).
  PlaceholderRange ext_ph = PlaceholderRange::reserve(delta, ext_addr);
  if (!ext_ph || ext_ph.base() != ext_addr) {
    // ~ext_ph releases if reserve returned a different address.
    windows::g_mmap_lock.release_exclusive();
    windows::g_mapping_table.abort_remap(addr);
    return 0;
  }

  uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);

  if (entry.spec.file) {
    // ── File-backed: extend + coalesce + single remap ──
    RegionRecord records[MAX_REGION_RECORDS];
    int rec_count = windows::snapshot_regions(addr_val, addr_val + old_size,
                                              records,
                                              MAX_REGION_RECORDS);
    if (LIBC_UNLIKELY(rec_count < 0)) {
      // ~ext_ph releases the extension placeholder.
      windows::g_mmap_lock.release_exclusive();
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    // WSEX per-page COW scan + save dirty page content.
    windows::CowContext cow_ctx;
    if (!cow_ctx.prepare(addr_val, records, rec_count)) {
      // ~ext_ph releases the extension placeholder.
      windows::g_mmap_lock.release_exclusive();
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    SIZE_T ext_max = static_cast<SIZE_T>(entry.spec.offset.QuadPart) + new_size;
    NTSTATUS st = nt_helpers::extend_section(entry.spec.section, ext_max);
    if (NT_ERROR(st)) {
      // cow_ctx RAII handles cleanup.
      // ~ext_ph releases the extension placeholder.
      windows::g_mmap_lock.release_exclusive();
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    // Unmap old view → placeholder.
    st = ::NtUnmapViewOfSectionEx(process, addr,
                                   MEM_UNMAP_WITH_TRANSIENT_BOOST |
                                   MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
    if (NT_ERROR(st)) {
      // cow_ctx RAII handles cleanup.
      // ~ext_ph releases the extension placeholder.
      windows::g_mmap_lock.release_exclusive();
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    // Coalesce into single placeholder.
    PVOID coalesce_base = addr;
    SIZE_T coalesce_size = new_size;
    st = ::NtFreeVirtualMemory(process, &coalesce_base, &coalesce_size,
                                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    if (NT_ERROR(st)) {
      // Recover: remap old view at original size.
      entry.spec.map_into(addr, old_size);
      if (rec_count > 0)
        windows::replay_protections(process, addr_val, entry.spec.prot,
                                    records, rec_count);
      cow_ctx.restore(addr_val, records, rec_count);
      // ~ext_ph releases the extension placeholder.
      windows::g_mmap_lock.release_exclusive();
      // View restored successfully — abort back to LIVE.
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    // Coalesce succeeded — ext_ph's VA is now part of the coalesced
    // placeholder [addr, addr+new_size). Must consume to prevent the
    // destructor from releasing a merged address.
    (void)ext_ph.consume();

    // Map extended section as single view.
    st = entry.spec.map_into(addr, new_size);
    if (NT_ERROR(st)) {
      // Recover: split back, remap old view.
      windows::split_placeholder(addr, old_size);
      entry.spec.map_into(addr, old_size);
      if (rec_count > 0)
        windows::replay_protections(process, addr_val, entry.spec.prot,
                                    records, rec_count);
      cow_ctx.restore(addr_val, records, rec_count);
      PVOID ep = ext_addr;
      SIZE_T es = 0;
      ::NtFreeVirtualMemory(process, &ep, &es, MEM_RELEASE);
      windows::g_mmap_lock.release_exclusive();
      // View restored — abort back to LIVE.
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    if (rec_count > 0)
      windows::replay_protections(process, addr_val, entry.spec.prot,
                                  records, rec_count);
    cow_ctx.restore(addr_val, records, rec_count);

    windows::g_mmap_lock.release_exclusive();

    // Commit: REMAPPING -> LIVE with the extended view.
    windows::g_mapping_table.commit_remap(
        addr, addr, new_size, entry.spec);
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
      windows::g_mmap_lock.release_exclusive();
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    // Map extension section into the extension placeholder.
    LARGE_INTEGER ext_offset = {};
    windows::ViewSpec ext_spec{ext_section, /*file=*/nullptr, ext_offset,
                               entry.spec.prot, windows::VM_FLAG_SEC_RESERVE};
    st = ext_spec.map_into(ext_addr, delta);
    if (NT_ERROR(st)) {
      ::NtClose(ext_section);
      // ~ext_ph releases the extension placeholder.
      windows::g_mmap_lock.release_exclusive();
      windows::g_mapping_table.abort_remap(addr);
      return 0;
    }

    // map_into succeeded — placeholder consumed by the view.
    (void)ext_ph.consume();

    windows::g_mmap_lock.release_exclusive();

    // Restore original entry (old view was never unmapped) and register
    // the extension as a separate entry.
    windows::g_mapping_table.abort_remap(addr);
    windows::g_mapping_table.register_mapping_take(
        ext_addr, delta, ext_spec);
    return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
  }
}

/// Grow a bare placeholder in-place by coalescing with adjacent free VA.
/// Returns address-as-long on success, 0 if not possible.
intptr_t grow_bare_placeholder_in_place(void *addr, SIZE_T old_size,
                                                 SIZE_T new_size) {
  HANDLE process = NtCurrentProcess();
  SIZE_T delta = new_size - old_size;
  char *ext_addr = static_cast<char *>(addr) + old_size;

  if (!windows::is_range_free(ext_addr, delta))
    return 0;

  PlaceholderRange ext = PlaceholderRange::reserve(delta, ext_addr);
  if (!ext || ext.base() != ext_addr) {
    // ~ext releases if reserve returned a different address.
    return 0;
  }

  // Coalesce into single placeholder [base, base+new_size).
  PVOID coalesce_base = addr;
  SIZE_T coalesce_size = new_size;
  NTSTATUS st = ::NtFreeVirtualMemory(process, &coalesce_base, &coalesce_size,
                                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
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
  if (!windows::split_placeholder(addr, new_size)) {
    return -ENOMEM;
  }

  char *tail = static_cast<char *>(addr) + new_size;
  PVOID tail_base = tail;
  SIZE_T tail_size = 0;
  ::NtFreeVirtualMemory(NtCurrentProcess(), &tail_base, &tail_size,
                         MEM_RELEASE);
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
    if (int err = windows::validate_map_fixed_target(fixed_addr, new_size)) {
      return -err;
    }
    windows::g_mmap_lock.acquire_exclusive();
    if (!windows::prepare_for_fixed(fixed_addr, new_size)) {
      windows::g_mmap_lock.release_exclusive();
      return -ENOMEM;
    }
    if (windows::is_alloc_aligned(fixed_addr)) {
      new_ph = PlaceholderRange::from_raw(fixed_addr, new_size);
    } else {
      new_ph =
          PlaceholderRange::reserve_exact_with_retry(new_size, fixed_addr);
    }
    windows::g_mmap_lock.release_exclusive();

    if (!new_ph) {
      return -ENOMEM;
    }
  } else {
    new_ph = PlaceholderRange::reserve(new_size);
    if (!new_ph) {
      return -ENOMEM;
    }
  }

  if (!dontunmap) {
    PVOID old_base = old_addr;
    SIZE_T sz = 0;
    ::NtFreeVirtualMemory(NtCurrentProcess(), &old_base, &sz, MEM_RELEASE);
  }
  // dontunmap: old placeholder stays as-is (already inaccessible).

  // Bare placeholder stays reserved — consume to transfer ownership to caller.
  auto result = new_ph.consume();
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(result.base));
}

//==========================================================================
// Private committed memory operations (MAP_ANONYMOUS without MAP_NORESERVE)
//==========================================================================

/// Grow private committed memory in-place by allocating adjacent VA.
/// Uses oneshot_alloc at the extension address — single syscall if the
/// VA is free and 64KB-aligned. Returns address-as-long on success,
/// 0 if not possible (VA occupied or alignment mismatch).
intptr_t grow_private_in_place(void *addr, SIZE_T old_size,
                                        SIZE_T new_size) {
  SIZE_T delta = new_size - old_size;
  char *ext_addr = static_cast<char *>(addr) + old_size;

  // One-shot alloc at the exact extension address.
  void *base = windows::oneshot_alloc(ext_addr, delta, PAGE_READWRITE);
  if (!base)
    return 0; // VA occupied or kernel chose a different address.
  if (base != ext_addr) {
    // Kernel placed it elsewhere — extension not contiguous. Release.
    windows::vm_release(base);
    return 0;
  }
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
}

/// Shrink private committed memory by releasing the tail to MEM_FREE.
/// Single syscall via interior_release. Returns address-as-long on
/// success, -errno on failure.
intptr_t shrink_private(void *addr, SIZE_T old_size,
                                 SIZE_T new_size) {
  char *tail = static_cast<char *>(addr) + new_size;
  SIZE_T tail_size = old_size - new_size;

  // Interior release: tail becomes MEM_FREE in one syscall.
  // Replaces the three-step decommit+preserve+release sequence.
  if (!windows::interior_release(tail, tail_size))
    return -ENOMEM;
  return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(addr));
}

/// Move private committed memory to a new address via memcpy.
/// No zero-copy — private memory can't remap sections.
///
/// Uses oneshot_alloc for the new allocation (single syscall) when
/// non-fixed. Fixed targets use prepare_for_fixed + oneshot/placeholder.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t move_private(void *old_addr, SIZE_T old_size,
                               SIZE_T new_size, void *fixed_addr,
                               bool dontunmap) {
  void *new_base = nullptr;

  if (fixed_addr) {
    // MREMAP_FIXED: tear down target VA and allocate there.
    // prepare_for_fixed leaves: placeholder (64KB-aligned) or MEM_FREE.
    if (int err = windows::validate_map_fixed_target(fixed_addr, new_size))
      return -err;

    windows::g_mmap_lock.acquire_exclusive();
    if (!windows::prepare_for_fixed(fixed_addr, new_size)) {
      windows::g_mmap_lock.release_exclusive();
      return -ENOMEM;
    }
    windows::g_mmap_lock.release_exclusive();

    // Adopt or create placeholder and commit into it.
    PlaceholderRange new_ph;
    if (windows::is_alloc_aligned(fixed_addr)) {
      new_ph = PlaceholderRange::from_raw(fixed_addr, new_size);
    } else {
      new_ph =
          PlaceholderRange::reserve_exact_with_retry(new_size, fixed_addr);
    }
    if (!new_ph)
      return -ENOMEM;

    new_base = new_ph.base();
    NTSTATUS st = new_ph.commit(PAGE_READWRITE);
    if (NT_ERROR(st))
      return -ENOMEM; // ~new_ph releases placeholder.
  } else {
    // Non-fixed: oneshot at system-chosen address.
    new_base = windows::oneshot_alloc(nullptr, new_size, PAGE_READWRITE);
    if (!new_base)
      return -ENOMEM;
  }

  // Copy old data.
  SIZE_T copy_size = (old_size < new_size) ? old_size : new_size;
  __builtin_memcpy(new_base, old_addr, copy_size);

  // Free or preserve old allocation.
  if (!dontunmap)
    windows::vm_release(old_addr);

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

  // Determine region type.
  MEMORY_BASIC_INFORMATION mbi;
  if (!windows::query_region(old_address, mbi) || mbi.State == MEM_FREE) {
    return -EINVAL;
  }

  if (mbi.Type == MEM_IMAGE) {
    return -EINVAL;
  }

  const bool is_section = (mbi.Type == MEM_MAPPED);
  const bool is_bare_placeholder =
      (mbi.Type == MEM_PRIVATE && mbi.State == MEM_RESERVE);
  const bool is_private_committed =
      (mbi.Type == MEM_PRIVATE && mbi.State == MEM_COMMIT);
  const bool dontunmap = (flags & MREMAP_DONTUNMAP);

  if (!is_section && !is_bare_placeholder && !is_private_committed) {
    return -EINVAL;
  }

  // MREMAP_DONTUNMAP: only valid for anonymous mappings (null file_handle).
  if (dontunmap && is_section) {
    windows::SlotSnapshot snap;
    if (!windows::g_mapping_table.snapshot(old_address, &snap))
      return -EINVAL;
    if (snap.file_handle != nullptr)
      return -EINVAL;
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
