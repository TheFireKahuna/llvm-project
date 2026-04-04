//===-- Windows memory region operations -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Placeholder-based memory operations for POSIX mmap emulation on Windows.
//
// POSIX mmap/munmap semantics that have no direct Windows equivalent:
//   - munmap can partially unmap any page-aligned subrange
//   - MAP_FIXED atomically replaces existing mappings
//   - mmap returns addresses whose subranges can be independently unmapped
//
// Windows placeholders (MEM_RESERVE_PLACEHOLDER) bridge the gap:
//   - Split reserved regions at arbitrary page-aligned boundaries
//   - Replace placeholders with committed memory or file views
//   - Convert committed memory back to placeholders for resplitting
//
// All operations use NT APIs directly (NtAllocateVirtualMemoryEx,
// NtCreateSectionEx, NtMapViewOfSectionEx, NtFreeVirtualMemory,
// NtQueryVirtualMemory, NtProtectVirtualMemory) — no Win32 memory APIs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/OSUtil/windows/memory/region_reconcile.h"
#include "src/__support/OSUtil/windows/alloc/placeholder_range.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/numa_policy.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Region State Classification
//===----------------------------------------------------------------------===//

/// Region type determines the teardown strategy in prepare_for_fixed.
enum class RegionType {
  Free,           ///< Unallocated address space
  Private,        ///< VirtualAlloc-based private memory
  MappedFile,     ///< File mapping view (data file)
  MappedImage,    ///< Executable image mapping (cannot be replaced)
  MappedPageFile, ///< Pagefile-backed section (e.g., ring buffer)
};

/// Region metadata from NtQueryVirtualMemory(MemoryRegionInformationEx).
/// Must use info class 7 (Ex), not 3 — class 3 returns raw allocation type
/// constants in RegionType instead of the proper bitfield, so fields like
/// PlaceholderReservation are always zero.
struct RegionInfo {
  RegionType type;
  void *alloc_base;     ///< Base of the containing allocation
  SIZE_T region_size;   ///< Size of contiguous region with same attributes
  SIZE_T commit_size;   ///< Committed portion (0 = reserved only)
  ULONG alloc_protect;  ///< Original allocation protection
  bool is_committed;    ///< True if commit_size > 0

  // NT-only extended fields (not in Win32 wrapper).
  bool is_placeholder;
  bool is_large_page;
  bool is_64k_page;
  bool is_enclave;
  bool is_awe;
  bool is_write_watch;
  ULONG_PTR numa_node;
  ULONG_PTR partition_id;
};

/// Returns false for free/unallocated memory or query failure, zeroing *info.
LIBC_INLINE bool query_region(const void *addr, RegionInfo *info) {
  MEMORY_REGION_INFORMATION mri;
  SIZE_T return_size;

  NTSTATUS status = ::NtQueryVirtualMemory(
      NtCurrentProcess(), const_cast<void *>(addr), MemoryRegionInformationEx,
      &mri, sizeof(mri), &return_size);

  if (NT_ERROR(status)) {
    // NtQueryVirtualMemory returns STATUS_INVALID_PARAMETER for free regions.
    if (info) {
      info->type = RegionType::Free;
      info->alloc_base = nullptr;
      info->region_size = 0;
      info->commit_size = 0;
      info->alloc_protect = 0;
      info->is_committed = false;
      info->is_placeholder = false;
      info->is_large_page = false;
      info->is_64k_page = false;
      info->is_enclave = false;
      info->is_awe = false;
      info->is_write_watch = false;
      info->numa_node = 0;
      info->partition_id = 0;
    }
    return false;
  }

  if (info) {
    if (mri.MappedImage)
      info->type = RegionType::MappedImage;
    else if (mri.MappedDataFile || mri.DirectMapped)
      info->type = RegionType::MappedFile;
    else if (mri.MappedPageFile || mri.MappedPhysical)
      info->type = RegionType::MappedPageFile;
    else if (mri.Private)
      info->type = RegionType::Private;
    else
      info->type = RegionType::Free;

    info->alloc_base = mri.AllocationBase;
    info->region_size = mri.RegionSize;
    info->commit_size = mri.CommitSize;
    info->alloc_protect = mri.AllocationProtect;
    info->is_committed = (mri.CommitSize > 0);

    info->is_placeholder = mri.PlaceholderReservation != 0;
    info->is_large_page = (mri.PageSizeLarge != 0) || (mri.PageSizeHuge != 0);
    info->is_64k_page = mri.PageSize64K != 0;
    info->is_enclave = mri.SoftwareEnclave != 0;
    info->is_awe = mri.MappedAwe != 0;
    info->is_write_watch = mri.MappedWriteWatch != 0;
    info->numa_node = mri.NodePreference;
    info->partition_id = mri.PartitionId;
  }

  return true;
}

/// True if addr is unallocated address space.
LIBC_INLINE bool is_free(const void *addr) {
  RegionInfo info;
  return !query_region(addr, &info);
}

/// True if addr is at its allocation base (required for MEM_RELEASE).
LIBC_INLINE bool is_at_alloc_base(const void *addr) {
  RegionInfo info;
  if (!query_region(addr, &info))
    return true; // Free memory has no base to compare against.
  return (addr == info.alloc_base);
}

// Placeholder operations and vm_* primitives are in memory_primitives.h.

//===----------------------------------------------------------------------===//
// MAP_FIXED Support
//===----------------------------------------------------------------------===//



/// Free all existing allocations in [addr, addr+size) so the range can be
/// reused by a new MAP_FIXED allocation.
///
/// Unlike Linux MAP_FIXED (kernel-atomic), this is a multi-step userspace
/// operation. Callers needing atomicity must use external synchronization.
///
/// Cases (target = MAP_FIXED range, X = existing allocation):
///
///   Case 1: Allocation fully contained -- release outright.
///     Target: |============================|
///     Alloc:       |XXXXXXXX|
///     After:  |........free................|
///
///   Case 2: Allocation starts at target base, extends past end.
///     Target: |============|
///     Alloc:  |XXXXXXXXXXXXXXXXXXXXX|
///     After:  |....free....|placeholder...|
///
///   Case 3: Target starts mid-allocation (DATA LOSS -- see above).
///     Before: |DDDDD|XXXXXXXX|DDDDD|        D = data we don't target
///     Target:       |XXXXXXXX|
///     Step 1: |pppppppppppppppppppp|         placeholder (all data lost)
///     Step 2: |ppppp|XXXXXXXX|ppppp|         split off prefix and suffix
///     Step 3: |ppppp|..free..|ppppp|         release target portion
///
///   Case 4: File/pagefile view -- NtUnmapViewOfSectionEx (unmaps entire view).
///   Case 5: Image mapping -- fail (cannot replace).
///

/// Release every tracked region in [addr, addr+size) back to MEM_FREE.
///
/// Iterates the mapping table — the only memory we are entitled to tear
/// down — and skips foreign content. Replaces the previous MBI-driven
/// implementation, which would happily MEM_RELEASE NT loader / heap /
/// thread-pool reservations along with our own. Best-effort: individual
/// release failures don't abort the sweep.
LIBC_INLINE void release_range_placeholders(void *addr, SIZE_T size) {
  char *end = static_cast<char *>(addr) + size;
  for (;;) {
    // Pick the first tracked slot in the range, then mutate it. Walk_range
    // doesn't survive concurrent table mutation cleanly, so each pass is
    // collect-one + extract-one rather than mutating from inside the
    // callback.
    struct PickFirst {
      void *base;
      uint32_t region_id;
      uint8_t alloc_id;
      bool found;
    } pick{nullptr, 0, 0, false};
    g_mapping_table.walk_range(
        addr, end,
        +[](const SlotSnapshot *snap, void *vctx) {
          auto &p = *static_cast<PickFirst *>(vctx);
          if (p.found || snap->region == nullptr)
            return;
          // Skip any shape whose VA we are not entitled to release:
          // LIBC_INTERNAL (libc allocator VA), IMAGE_REGION (loaded
          // modules), KERNEL_REGION (TEB/PEB/stack), FOREIGN_SENTINEL
          // (NT heap / third-party). Pass 0 should have already
          // rejected these; this is defence-in-depth so a misrouted
          // call cannot accidentally teardown non-user VA.
          if (snap->region->blocks_map_fixed())
            return;
          p.base = snap->view_base;
          p.region_id = snap->region_id;
          p.alloc_id = snap->alloc_id;
          p.found = true;
        },
        &pick);
    if (!pick.found)
      return;

    MappingEntry mentry;
    if (!g_mapping_table.extract(pick.base, &mentry))
      continue; // raced against another extractor; advance and re-probe.

    // Tear down the NT-side state. release_placeholder works for both
    // unmapped section views and bare placeholder reservations; vm_release
    // covers anonymous shapes. Choosing by shape avoids a wrong-API call
    // that would leave NT and table out of step.
    auto *rd = memory::g_region_pool.resolve(mentry.region_id, mentry.alloc_id);
    if (rd != nullptr && rd->section_handle != nullptr) {
      (void)unmap_view_preserve(pick.base);
      (void)release_placeholder(pick.base);
    } else {
      (void)vm_release(pick.base);
    }
    if (mentry.region_id != memory::RegionPool::NONE)
      memory::g_region_pool.release(mentry.region_id);
  }
}

/// Prepare [addr, addr+size) for MAP_FIXED by tearing down all existing
/// allocations and views.
///
/// Pass 1 — Teardown: MEM_PRIVATE regions are converted to placeholders
/// via decommit + preserve_to_placeholder (no MEM_FREE window during
/// teardown). MEM_MAPPED views are unmapped and released to MEM_FREE.
/// MEM_IMAGE regions cause immediate failure.
///
/// Pass 2 — Placeholder assembly (64KB-aligned addr only):
/// Fills MEM_FREE gaps with placeholders. Gaps at non-64KB boundaries
/// (between allocations with non-64KB-aligned sizes) are handled via
/// merge-back: release the preceding placeholder run back to its 64KB
/// start, then create one bigger placeholder spanning the merged range.
/// Finally, coalesces all placeholders into one contiguous range.
///
/// On success with 64KB-aligned addr: the range is a single contiguous
/// placeholder — callers use PlaceholderRange::from_raw() (deterministic).
///
/// On success with non-64KB addr: the range is MEM_FREE — callers use
/// reserve_with_retry() (legacy contract, only for alloc_file_fixed
/// non-aligned path).
[[nodiscard]] LIBC_INLINE bool prepare_for_fixed(void *addr, SIZE_T size) {
  char *start = static_cast<char *>(addr);
  char *end = start + size;

  // ── Pass 0: validate via the mapping table ──
  //
  // Reject any range overlapping a shape we refuse to displace:
  //
  //   LIBC_INTERNAL   — allocator VA owned by this libc. MAP_FIXED
  //                     here would corrupt slab pools / the mapping
  //                     table's own metadata.
  //   IMAGE_REGION    — loaded module (c.dll, ntdll, exe, DLLs).
  //                     MAP_FIXED would overlay executable code.
  //   KERNEL_REGION   — TEB / PEB / main-thread stack reservation.
  //   FOREIGN_SENTINEL — NT heap, debugger ranges, third-party
  //                     VirtualAllocEx allocations — VA we do not own.
  //
  // All four yield `-EINVAL` at the POSIX boundary. The shape tag
  // distinguishes them for diagnostics, but the caller sees one
  // consistent refusal code.
  struct PassZero {
    bool blocked;
  } phase0{false};
  g_mapping_table.walk_range(
      start, end,
      +[](const SlotSnapshot *snap, void *vctx) {
        auto &c = *static_cast<PassZero *>(vctx);
        if (c.blocked || snap->region == nullptr)
          return;
        if (snap->region->blocks_map_fixed())
          c.blocked = true;
      },
      &phase0);
  if (phase0.blocked)
    return false;

  // ── Pass 1: tear down every tracked region in the range ──
  //
  // Single helper, table-driven: it picks slots one at a time, dispatches
  // teardown by shape (anon → vm_release, section-backed →
  // unmap_view_preserve + release_placeholder), and skips FOREIGN. The
  // resulting NT state is MEM_FREE everywhere we owned, plus whatever
  // pre-existing MEM_FREE / foreign-untracked content was already in the
  // range.
  release_range_placeholders(addr, size);

  // ── Pass 2: Placeholder assembly ──
  //
  // For 64KB-aligned addr: fill gaps + coalesce → contiguous placeholder.
  // For non-64KB addr: release all placeholders → MEM_FREE.

  if (!is_alloc_aligned(addr)) {
    release_range_placeholders(addr, size);
    return true;
  }

  // Authoritatively populate the table for [start, end): one bulk syscall
  // (`NtPssCaptureVaSpaceBulk`) walks every NT VAD in the range and
  // stamps FOREIGN over anything we don't own. After this returns, the
  // mapping table is the single source of truth for the question
  // Pass 2 actually wants to ask: "is this granule available for me to
  // create_placeholder over?"
  //
  // Plan-coherent: this is the same reconciliation primitive used at
  // fork / exec / dlopen, just scoped to the prepare_for_fixed range.
  // Cost: O(1) syscalls regardless of region count, vs the previous
  // per-region MBI walk (O(N) syscalls). Side benefit: the FOREIGN
  // stamps survive past prepare_for_fixed, so subsequent operations
  // against this VA read the table without re-probing NT.
  //
  // TODO(Phase D, option 3): if every foreign-VA-creation event (DLL
  // load, heap grow, thread-pool stack alloc, user VirtualAlloc2) ever
  // gets a hook that updates the table on creation, this scoped
  // reconciliation becomes redundant — the table would already know.
  // Until then, the bulk-scan-and-stamp on demand is the correct trade.
  (void)memory::cordon_foreigners_in_range(addr, size);

  // Fill MEM_FREE gaps via the mapping table. walk_range hands us every
  // owned/cordoned slot in order; the gaps between consecutive callbacks
  // (and at the ends) are the FREE granules we must turn into
  // placeholders. run_start tracks the 64KB-aligned start of the
  // contiguous placeholder run being built; merge-back handles the
  // (rare) case where a gap starts at a non-64KB boundary because some
  // surviving allocation isn't 64KB-multiple in size.
  struct GapFill {
    char *cursor;     // first byte not yet covered by a placeholder/owned slot
    char *end;        // exclusive request end
    char *run_start;  // 64KB-aligned start of current placeholder run
    bool ok;          // false on the first failed create_placeholder
  };
  GapFill gf{start, end, nullptr, true};

  // Stateless: implicitly converts to a function pointer.
  auto fill_gap = +[](GapFill &g, char *gap_end) {
    if (!g.ok || g.cursor >= gap_end)
      return;
    SIZE_T gap_size = static_cast<SIZE_T>(gap_end - g.cursor);
    if (is_alloc_aligned(g.cursor)) {
      if (!create_placeholder(g.cursor, gap_size))
        g.ok = false;
    } else {
      // Non-64KB gap — merge back with the preceding placeholder run.
      SIZE_T run_len = static_cast<SIZE_T>(g.cursor - g.run_start);
      release_range_placeholders(g.run_start, run_len);
      SIZE_T merged_size = static_cast<SIZE_T>(gap_end - g.run_start);
      if (!create_placeholder(g.run_start, merged_size))
        g.ok = false;
    }
    if (!g.run_start)
      g.run_start = g.cursor;
  };

  struct WalkCtx {
    GapFill *gf;
    void (*fill)(GapFill &, char *);
  };
  WalkCtx wctx{&gf, fill_gap};

  g_mapping_table.walk_range(
      start, end,
      +[](const SlotSnapshot *snap, void *vctx) {
        auto &c = *static_cast<WalkCtx *>(vctx);
        char *slot_base = static_cast<char *>(snap->view_base);
        char *slot_end = slot_base + snap->view_size;
        // Clip to request range.
        if (slot_base < c.gf->cursor)
          slot_base = c.gf->cursor;
        if (slot_end > c.gf->end)
          slot_end = c.gf->end;
        if (slot_end <= c.gf->cursor)
          return;
        // Fill the FREE gap between cursor and this slot's start.
        c.fill(*c.gf, slot_base);
        // Slot itself is owned-or-cordoned; treat as part of the run.
        if (!c.gf->run_start)
          c.gf->run_start = slot_base;
        if (slot_end > c.gf->cursor)
          c.gf->cursor = slot_end;
      },
      &wctx);

  if (!gf.ok)
    return false;

  // Trailing gap from the last slot to `end`.
  fill_gap(gf, end);
  if (!gf.ok)
    return false;

  // Coalesce all adjacent placeholders into one.
  NTSTATUS st = vm_coalesce_placeholders(addr, size);
  if (NT_ERROR(st))
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
// NUMA-Aware Allocation
//===----------------------------------------------------------------------===//


/// Returns the preferred NUMA node for addr, or 0 if unknown.
LIBC_INLINE ULONG_PTR get_numa_node(const void *addr) {
  RegionInfo info;
  if (query_region(addr, &info))
    return info.numa_node;
  return 0;
}

//===----------------------------------------------------------------------===//
// Ring Buffer (Double-Mapped Circular Buffer)
//===----------------------------------------------------------------------===//

/// Create a double-mapped ring buffer: [0, size) and [size, 2*size) alias the
/// same physical pages, enabling contiguous reads across the wrap boundary.
/// Size must be page-aligned.
///
/// The two views share a single pagefile-backed section. Both are
/// published into the mapping table as LIVE slots referencing the same
/// region_id with refcount = 2 — the section handle is owned by the
/// region descriptor and stays open until BOTH slots are torn down. This
/// makes ring buffers a first-class citizen of the region/shape model:
/// snapshot, walk_range, fork CoW, and reconciliation all see them as a
/// single logical mapping spanning two adjacent VA windows.
LIBC_INLINE void *create_ring_buffer(SIZE_T size) {
  if (size == 0 || (size & (get_page_size() - 1)) != 0)
    return nullptr;
  if (size > SIZE_MAX / 2)
    return nullptr;

  // Reserve 2x VA as a single placeholder, then split at midpoint.
  PlaceholderRange whole = PlaceholderRange::reserve(2 * size);
  if (!whole)
    return nullptr;

  PlaceholderRange left, right;
  if (!whole.split(size, &left, &right))
    return nullptr; // ~whole releases the 2x reservation.

  // Pagefile-backed section provides the shared physical storage.
  LARGE_INTEGER section_size;
  section_size.QuadPart = static_cast<LONGLONG>(size);

  HANDLE section = nullptr;
  auto sec_oa = internal_oa();
  NTSTATUS status = ::NtCreateSectionEx(
      &section, SECTION_MAP_READ | SECTION_MAP_WRITE, &sec_oa, &section_size,
      PAGE_READWRITE, SEC_COMMIT, nullptr, nullptr, 0);
  if (NT_ERROR(status))
    return nullptr; // ~left + ~right release both placeholders.

  // Hand the section handle to the region pool. The pool owns it from
  // here on and will close it when the region's refcount drops to zero.
  // Bounds cover both views so walk_range / has_neighbor_* recognize the
  // full ring as one logical region. SHARED flag matches the underlying
  // SEC_COMMIT semantics (writes are visible across all mappers).
  void *primary = left.base();
  void *alias = right.base();
  memory::AcquireSpec spec{};
  spec.section_handle = section;
  spec.file_handle = nullptr;
  spec.section_offset = LARGE_INTEGER{};
  spec.shape = memory::RegionShape::FILE_VIEW_MONO;
  spec.flags = memory::region_flag::SHARED;
  spec.first_slot_key = reinterpret_cast<uintptr_t>(primary) >> 16;
  spec.last_slot_key =
      (reinterpret_cast<uintptr_t>(primary) + 2 * size) >> 16;
  memory::RegionTicket ticket = memory::g_region_pool.reserve(spec);
  if (!ticket) {
    ::NtClose(section);
    return nullptr; // ~placeholders release VA.
  }
  // Pool owns `section` now; on ticket drop / region release it is closed.

  HANDLE current_process = NtCurrentProcess();

  // Map both views. MEM_REPLACE_PLACEHOLDER atomically swaps the
  // placeholder for a section view backed by the same physical pages.
  LARGE_INTEGER offset_zero = {};
  PVOID base1 = primary;
  SIZE_T view_size1 = size;
  status = ::NtMapViewOfSectionEx(section, current_process, &base1,
                                   &offset_zero, &view_size1,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                   nullptr, 0);
  if (NT_ERROR(status))
    return nullptr; // ~ticket releases region (closes section); ~halves VA.
  (void)left.consume();

  LARGE_INTEGER offset_zero2 = {};
  PVOID base2 = alias;
  SIZE_T view_size2 = size;
  status = ::NtMapViewOfSectionEx(section, current_process, &base2,
                                   &offset_zero2, &view_size2,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                   nullptr, 0);
  if (NT_ERROR(status)) {
    unmap_view(base1);
    return nullptr; // ~ticket releases region; ~right releases VA.
  }
  (void)right.consume();

  // Publish the primary slot. This consumes the ticket's +1 reference
  // (refcount stays at 1; ownership of that ref now lives in the slot).
  if (!g_mapping_table.register_mapping(base1, size, ticket.region_id(),
                                        ticket.alloc_id(), PAGE_READWRITE,
                                        /*flags=*/0)) {
    unmap_view(base1);
    unmap_view(base2);
    return nullptr; // ~ticket releases region (closes section).
  }
  const uint32_t region_id = ticket.commit();
  const uint8_t alloc_id =
      memory::g_region_pool.get_mutable(region_id)->alloc_id.load(
          cpp::MemoryOrder::RELAXED);

  // Add the second reference for the alias slot, then publish. add_ref is
  // lock-free; we are guaranteed not to hit the last-ref path.
  memory::g_region_pool.add_ref(region_id);
  if (!g_mapping_table.register_mapping(base2, size, region_id, alloc_id,
                                        PAGE_READWRITE, /*flags=*/0)) {
    // Roll back: drop the alias add_ref (refcount 2→1, no lock needed),
    // then remove the primary slot. The primary remove drops the last
    // ref, which triggers section close — that release path requires
    // MmapLock writer per RegionPool::release contract.
    memory::g_region_pool.release(region_id);
    {
      MmapLockWriterGuard guard;
      g_mapping_table.remove(base1);
    }
    unmap_view(base1);
    unmap_view(base2);
    return nullptr;
  }

  return primary;
}

/// Destroy a ring buffer created by create_ring_buffer. Each remove drops
/// one region reference; the second drop closes the shared section
/// handle. The writer lock brackets both removes so the last-ref teardown
/// inside RegionPool::release runs without racing any concurrent reader
/// dereferencing the descriptor.
LIBC_INLINE void destroy_ring_buffer(void *ring, SIZE_T size) {
  if (!ring)
    return;
  void *primary = ring;
  void *alias = static_cast<char *>(ring) + size;

  unmap_view(primary);
  unmap_view(alias);

  MmapLockWriterGuard guard;
  g_mapping_table.remove(primary);
  g_mapping_table.remove(alias);
}

//===----------------------------------------------------------------------===//
// Advisory VM Information (madvise/posix_madvise shared helper)
//===----------------------------------------------------------------------===//

// apply_vm_info_per_region → for_committed_batched (memory_primitives.h)
// prefetch_committed_regions → prefetch_committed (memory_primitives.h)

// reset_pages → vm_reset (memory_primitives.h)
// dontneed_private_pages → vm_dontneed (memory_primitives.h)

/// Query max page priority across a range via WSEX batched query.
/// Returns the highest priority value (0-7) across all resident pages.
/// Used by MADV_COLD, MADV_PAGEOUT, and wsex_dontneed_section_pages
/// to skip VmPagePriorityInformation syscall when pages are already cold.
LIBC_INLINE ULONG wsex_max_priority(void *addr, SIZE_T size) {
  constexpr int BATCH = 256;
  const SIZE_T PAGE = get_page_size();
  MEMORY_WORKING_SET_EX_INFORMATION batch[BATCH];

  ULONG max_prio = 0;
  char *cursor = static_cast<char *>(addr);
  char *end = cursor + size;

  while (cursor < end) {
    int n = 0;
    for (; n < BATCH && cursor < end; n++, cursor += PAGE) {
      batch[n].VirtualAddress = cursor;
      batch[n].VirtualAttributes.Flags = 0;
    }
    NTSTATUS st = ::NtQueryVirtualMemory(
        NtCurrentProcess(), nullptr, MemoryWorkingSetExInformation, batch,
        static_cast<SIZE_T>(n) * sizeof(batch[0]), nullptr);
    if (NT_ERROR(st))
      return 4; // Assume hot on failure.
    for (int i = 0; i < n; i++) {
      ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
      ULONG prio = static_cast<ULONG>((flags >> 24) & 7);
      if (prio > max_prio)
        max_prio = prio;
    }
  }
  return max_prio;
}

/// WSEX-selective MADV_DONTNEED for section views where MEM_DECOMMIT fails.
/// Derived from DiscardVirtualMemory disassembly but 2x faster: skips MBI
/// validation and NtUnlockVirtualMemory. Zeroes only pages with physical
/// backing (working set or standby list). Conditional priority demotion.
LIBC_INLINE void wsex_dontneed_section_pages(void *addr, SIZE_T size) {
  constexpr int BATCH = 256;
  const SIZE_T PAGE = get_page_size();
  MEMORY_WORKING_SET_EX_INFORMATION batch[BATCH];

  ULONG max_prio = 0;
  char *cursor = static_cast<char *>(addr);
  char *end = cursor + size;

  while (cursor < end) {
    int n = 0;
    char *batch_start = cursor;
    for (; n < BATCH && cursor < end; n++, cursor += PAGE) {
      batch[n].VirtualAddress = cursor;
      batch[n].VirtualAttributes.Flags = 0;
    }

    NTSTATUS st = ::NtQueryVirtualMemory(
        NtCurrentProcess(), nullptr, MemoryWorkingSetExInformation, batch,
        static_cast<SIZE_T>(n) * sizeof(batch[0]), nullptr);
    if (NT_ERROR(st)) {
      // Fallback: blind memset remainder (matches DiscardVirtualMemory).
      __builtin_memset(batch_start, 0,
                       static_cast<size_t>(end - batch_start));
      max_prio = 4;
      break;
    }

    for (int i = 0; i < n; i++) {
      ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
      // Skip pages with no physical backing.
      if (!(flags & 1) && (flags & 0xC00000) != 0x400000)
        continue;
      ULONG prio = static_cast<ULONG>((flags >> 24) & 7);
      if (prio > max_prio)
        max_prio = prio;
      __builtin_memset(batch[i].VirtualAddress, 0, PAGE);
    }
  }

  // MEM_RESET entire range — zeroed pages marked discardable.
  vm_reset(addr, size);

  // Conditional priority demotion — skip if already cold.
  if (max_prio > 1) {
    MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = addr;
    range.NumberOfBytes = size;
    MEMORY_PAGE_PRIORITY_INFORMATION prio = {MEMORY_PRIORITY_VERY_LOW};
    ::NtSetInformationVirtualMemory(NtCurrentProcess(),
                                    VmPagePriorityInformation, 1,
                                    &range, &prio, sizeof(prio));
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_H
