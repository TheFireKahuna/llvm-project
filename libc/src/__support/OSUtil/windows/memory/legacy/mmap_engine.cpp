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
// Large pages route through `nt_pal::reserve_large_pages` /
// `nt_pal::commit_replace_large` (Layer 0 PAL); MAP_HUGETLB returns
// `EPERM` on `!nt_pal::large_pages_available()` per design §11.2.
//
// NT syscalls go through `nt_pal::*` exclusively — section creation
// via `nt_pal::create_section_anon` / `_file`, view mapping via
// `nt_pal::map_section_replace`, placeholder lifecycle via
// `nt_pal::reserve_placeholder` / `free_placeholder` /
// `unmap_view_preserve`. The engine carries no direct `NtCreateSectionEx`
// or `NtMapViewOfSectionEx` calls.
//
//===----------------------------------------------------------------------===//

#include "mmap_engine.h"

#include "src/__support/CPP/bit.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/CPP/utility.h"
#include "include/llvm-libc-macros/fcntl-macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/legacy/placeholder_range.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/memory/legacy/anon_region_ops.h"
#include "src/__support/OSUtil/windows/memory/legacy/fixed_range_guard.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/remap_transaction.h"
#include "src/__support/OSUtil/windows/memory/posix/mlock_policy.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/memory/legacy/view_spec.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_snapshot.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

using PlaceholderRange = windows::PlaceholderRange;

// ===========================================================================
// Region acquisition helpers — every file/section view publishes through
// the RegionPool so the slot stores only `region_id` + `alloc_id`. The
// pool owns the section/file handle pair for the region's lifetime.
// ===========================================================================

LIBC_INLINE bool is_writecopy_prot(DWORD prot) {
  return prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
}

LIBC_INLINE uint16_t derive_region_flags(int mmap_flags, DWORD view_prot,
                                         ULONG sec_flags) {
  uint16_t f = 0;
  if (mmap_flags & MAP_SHARED)
    f |= windows::memory::region_flag::SHARED;
  if (is_writecopy_prot(view_prot))
    f |= windows::memory::region_flag::COW;
  if (sec_flags & (SEC_LARGE_PAGES | SEC_64K_PAGES))
    f |= windows::memory::region_flag::HUGE_PAGES;
  if (sec_flags & SEC_RESERVE)
    f |= windows::memory::region_flag::NORESERVE;
  return f;
}

LIBC_INLINE windows::memory::RegionShape
derive_region_shape(ULONG sec_flags) {
  if (sec_flags & SEC_RESERVE)
    return windows::memory::RegionShape::FILE_VIEW_RESERVE;
  return windows::memory::RegionShape::FILE_VIEW_MONO;
}

/// Acquire a region descriptor that owns `section` and `file`. On success
/// the returned region_id has refcount = 1 and the pool will close both
/// handles when the last ref drops. On failure (returns NONE) the caller
/// retains ownership of the handles.
[[nodiscard]] LIBC_INLINE uint32_t
acquire_region_owned(HANDLE section, HANDLE file, LARGE_INTEGER offset,
                     void *base, SIZE_T size,
                     windows::memory::RegionShape shape, uint16_t flags) {
  windows::memory::AcquireSpec spec{};
  spec.section_handle = section;
  spec.file_handle = file;
  spec.section_offset = offset;
  spec.shape = shape;
  spec.flags = flags;
  spec.first_slot_key = reinterpret_cast<uintptr_t>(base) >> 16;
  spec.last_slot_key = spec.first_slot_key + (size >> 16);
  return windows::memory::g_region_pool.acquire_owned(spec);
}

// Shared anon publish/trim helpers live in anon_region_ops.h so
// vm_protect.cpp and mmap_engine.cpp hit the same code path. Call
// `windows::publish_anon_committed`, `publish_anon_placeholder_protnone`,
// `publish_anon_placeholder_noreserve`, and `trim_anon_slot_for_hole`
// directly.

/// Publish a freshly mapped section view as a LIVE slot.
///
/// On success, the placeholder VA at `base` is now tracked under the new
/// region_id, and the pool owns both `section` and `file` (the caller MUST
/// NOT close them after a successful return).
///
/// On failure, the function consumes neither handle nor placeholder — the
/// caller still owns both and must clean up. This matches the surrounding
/// scope-guard discipline at every call site.
[[nodiscard]] bool
publish_view_register(void *base, SIZE_T size, HANDLE section, HANDLE file,
                      LARGE_INTEGER offset, DWORD view_prot, int mmap_flags,
                      ULONG sec_flags) {
  uint32_t rid = acquire_region_owned(
      section, file, offset, base, size, derive_region_shape(sec_flags),
      derive_region_flags(mmap_flags, view_prot, sec_flags));
  if (rid == windows::memory::RegionPool::NONE)
    return false;
  uint8_t aid =
      windows::memory::g_region_pool.get_mutable(rid)->alloc_id;
  if (!windows::g_mapping_table.register_mapping(base, size, rid, aid,
                                                 view_prot, /*flags=*/0)) {
    // Pool acquired the handles already — release reverts that.
    windows::memory::g_region_pool.release(rid);
    return false;
  }
  return true;
}

/// commit_remap variant: REMAPPING slot at `old_base` becomes LIVE at
/// `new_base` with a freshly acquired region. Same ownership transfer
/// rules as publish_view_register.
[[nodiscard]] bool
publish_view_commit(void *old_base, void *new_base, SIZE_T size, HANDLE section,
                    HANDLE file, LARGE_INTEGER offset, DWORD view_prot,
                    int mmap_flags, ULONG sec_flags) {
  uint32_t rid = acquire_region_owned(
      section, file, offset, new_base, size, derive_region_shape(sec_flags),
      derive_region_flags(mmap_flags, view_prot, sec_flags));
  if (rid == windows::memory::RegionPool::NONE)
    return false;
  uint8_t aid =
      windows::memory::g_region_pool.get_mutable(rid)->alloc_id;
  if (!windows::g_mapping_table.commit_remap(old_base, new_base, size, rid,
                                             aid, view_prot, /*flags=*/0)) {
    windows::memory::g_region_pool.release(rid);
    return false;
  }
  return true;
}

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
                                                   HANDLE *out_section,
                                                   nt_pal::LargePageKind kind);
intptr_t map_anon_private_into_placeholder(PlaceholderRange &ph,
                                                       int prot);
intptr_t map_anon_reserve_into_placeholder(PlaceholderRange &ph,
                                                       int prot);
intptr_t alloc_fixed_free(void *addr, SIZE_T size, int prot, int flags);
intptr_t alloc_fixed_free_commit(void *placeholder, SIZE_T size,
                                 int prot, int flags);
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
  if (!nt_pal::decommit_private_range(addr, size))
    return -EINVAL;

  // PROT_NONE: decommitted pages are already inaccessible. Done.
  if (prot == PROT_NONE)
    return cpp::bit_cast<intptr_t>(addr);

  // Recommit: zero-filled pages with the requested protection.
  DWORD page_prot = windows::prot_to_page_flags(prot);
  NTSTATUS st = nt_pal::commit_in_reservation_no_writewatch(addr, size, page_prot);
  if (NT_ERROR(st))
    return windows_util::ntstatus_to_kerr(st);

  return cpp::bit_cast<intptr_t>(addr);
}

/// MAP_FIXED across multiple anon regions, table-driven.
///
/// Two-phase teardown driven by the mapping table — the source of truth
/// for what we own. The previous bulk-MBI implementation re-derived
/// ownership from NT on every call; the table already knows.
///
/// Phase 0 — validate via `walk_range`. Any in-range slot whose shape is
/// not pure anon (FILE_VIEW_*, ANON_RESERVE_SECTION, FOREIGN_SENTINEL)
/// blocks: caller must re-dispatch (split-remap for file views, refusal
/// for foreign reservations).
///
/// Phase 1 under exclusive lock — for each in-range anon slot, extract +
/// release the VA. Full coverage uses MEM_RELEASE; partial coverage uses
/// `trim_anon_slot_for_hole` (re-publishes survivors) + `nt_pal::interior_release`
/// for the carved sub-range. Untracked gaps are assumed MEM_FREE — the
/// alloc-fixed-free path below surfaces any disagreement.
///
/// Phase 2 — `alloc_fixed_free` maps fresh into the now-vacant range.
intptr_t alloc_fixed_private_multi(void *addr, SIZE_T size, int prot) {
  char *start = static_cast<char *>(addr);
  char *end = start + size;

  // Phase 0: shape validation via the table. A walk_range pass clears any
  // in-flight WRITING window (snapshot stalls inside the callback) and
  // returns published shapes only.
  struct PhaseZero {
    bool blocked;
  } phase0{false};
  windows::g_mapping_table.walk_range(
      start, end,
      +[](const windows::SlotSnapshot *snap, void *vctx) {
        auto &c = *static_cast<PhaseZero *>(vctx);
        if (c.blocked || snap->region == nullptr)
          return;
        const auto sh = snap->region->current_shape();
        if (sh != windows::memory::RegionShape::ANON_PLACEHOLDER)
          c.blocked = true;
      },
      &phase0);
  if (phase0.blocked)
    return -ENOMEM; // caller redispatches (split-remap or refusal).

  // Phase 1 under exclusive lock: serialized teardown of each in-range
  // anon slot. The guard pins the table against concurrent publish/extract
  // so the per-cursor snapshot reflects committed state. Released before
  // Phase 2 since alloc_fixed_free does not require it.
  {
    windows::MmapLockWriterGuard lock_guard;

    char *cur = start;
    const SIZE_T gran = windows::get_alloc_granularity();
    while (cur < end) {
      char *aligned = reinterpret_cast<char *>(
          windows::align_down_to_granularity(reinterpret_cast<uintptr_t>(cur)));
      windows::SlotSnapshot snap;
      const bool have_slot =
          windows::g_mapping_table.snapshot(aligned, &snap) &&
          snap.region != nullptr;

      if (!have_slot) {
        // Untracked granule — presume MEM_FREE. alloc_fixed_free will
        // reject if NT actually has something there (foreign placeholder,
        // unmodelled state); the caller's retry loop reclassifies.
        char *next = aligned + gran;
        cur = (next > cur) ? next : (cur + gran);
        continue;
      }

      const auto shape = snap.region->current_shape();
      // Phase 0 already established no non-anon shapes. A late shape
      // promotion would have been blocked from publishing by our writer
      // lock acquisition window.
      LIBC_ASSERT(shape == windows::memory::RegionShape::ANON_PLACEHOLDER);
      (void)shape;

      char *region_base = static_cast<char *>(snap.view_base);
      char *region_end = region_base + snap.view_size;
      char *overlap_start = (cur > region_base) ? cur : region_base;
      char *overlap_end = (region_end < end) ? region_end : end;
      const SIZE_T overlap_size =
          static_cast<SIZE_T>(overlap_end - overlap_start);

      if (overlap_start <= region_base && overlap_end >= region_end) {
        // Full overlap: extract the slot, release the VA, drop our region
        // ref. Three steps — extract is the table mutation, nt_pal::free_va the
        // NT mutation, region_pool.release the descriptor refcount drop.
        windows::MappingEntry entry;
        if (!windows::g_mapping_table.extract(region_base, &entry))
          return -EINVAL;
        if (entry.region_id != windows::memory::RegionPool::NONE)
          windows::memory::g_region_pool.release(entry.region_id);
        if (!nt_pal::free_va(region_base))
          return -EINVAL;
      } else {
        // Partial overlap: re-publish surviving head/tail under fresh region
        // ids, then carve the sub-range. trim_anon_slot_for_hole owns the
        // table-side bookkeeping; nt_pal::interior_release owns the NT-side punch.
        (void)windows::trim_anon_slot_for_hole(
            region_base, reinterpret_cast<uintptr_t>(overlap_start),
            reinterpret_cast<uintptr_t>(overlap_end));
        if (!nt_pal::interior_release(overlap_start, overlap_size))
          return -EINVAL;
      }

      cur = (region_end < end) ? region_end : end;
    }
  }

  // Phase 2: range is vacant — standard MAP_FIXED-into-free path.
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
  // Find the containing view's base and end via the mapping table — the
  // table owns this answer. The MRI fallback that previously asked NT to
  // re-derive the containing view raced against concurrent partial-munmap
  // (the kernel could report bounds the table had already split). Snapshot
  // is the single source of truth: no syscall, alloc_id-validated.
  windows::SlotSnapshot view_snap;
  {
    char *aligned = reinterpret_cast<char *>(
        windows::align_down_to_granularity(reinterpret_cast<uintptr_t>(addr)));
    if (!windows::g_mapping_table.snapshot(aligned, &view_snap) ||
        view_snap.region == nullptr)
      return -EINVAL;
  }
  char *view_base = static_cast<char *>(view_snap.view_base);
  char *view_end = view_base + view_snap.view_size;

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

  if (txn.region() == nullptr || txn.region()->section_handle == nullptr) {
    // No section handle — can't split-remap (e.g., large pages).
    // ~txn will call abort_remap().
    return -EINVAL;
  }

  auto result = txn.execute();
  if (!result.target)
    return -ENOMEM; // ~txn rolls back.

  // Pre-reserve the RegionTicket for the new anon VA *before* mapping it
  // into the placeholder. This is the prepare-then-commit transaction
  // discipline: reservation OOM here triggers ~result.target (placeholder
  // released) + ~txn (split rolled back) — no kernel state survives the
  // failure. Once we move past this point the publish step is infallible
  // and the new VA becomes table-tracked atomically with the NT mutation.
  void *target_base = result.target.base();
  SIZE_T target_size = result.target.size();
  enum class AnonShape { Protnone, Committed, Noreserve };
  AnonShape new_shape;
  const windows::memory::RegionShape reserve_shape =
      windows::memory::RegionShape::ANON_PLACEHOLDER;
  uint16_t reserve_flags = 0;
  bool placeholder_state = false;
  if (prot == PROT_NONE) {
    new_shape = AnonShape::Protnone;
    placeholder_state = true;
  } else if (flags & MAP_NORESERVE) {
    new_shape = AnonShape::Noreserve;
    reserve_flags = windows::memory::region_flag::NORESERVE;
  } else {
    new_shape = AnonShape::Committed;
    reserve_flags = windows::memory::region_flag::COMMITTED;
  }

  windows::memory::RegionTicket pub_ticket =
      windows::anon_ops_detail::reserve_anon_region(
          target_base, target_size, reserve_shape, reserve_flags);
  if (!pub_ticket)
    return -ENOMEM; // ~result.target + ~txn unwind cleanly.

  // Now perform the irreversible NT mutation that creates the new mapping.
  intptr_t map_result;
  if (new_shape == AnonShape::Protnone) {
    map_result = cpp::bit_cast<intptr_t>(result.target.consume().base);
  } else if (new_shape == AnonShape::Noreserve) {
    map_result = map_anon_reserve_into_placeholder(result.target, prot);
  } else {
    // Committed
    map_result = map_anon_private_into_placeholder(result.target, prot);
  }

  if (LIBC_UNLIKELY(map_result < 0))
    return map_result; // ~result.target + ~pub_ticket release.

  if (!txn.commit())
    return -ENOMEM; // ~pub_ticket releases.

  // Publish the new anon VA. Cannot fail under the locking discipline:
  // target_base was just freed by the split (no concurrent slot owner)
  // and the radix expansion acquires a substrate sub-slot. A false return here
  // would indicate a real lock-contract violation — propagate as -ENOMEM
  // so stress tests fire loudly; ~pub_ticket still releases the region.
  DWORD page_prot = windows::prot_to_page_flags(prot);
  if (!windows::anon_ops_detail::commit_survivor_fragment(
          pub_ticket, target_base, target_size, page_prot, placeholder_state))
    return -ENOMEM;
  return map_result;
}

// freeze_bracket and promote_private_for_section were deleted in the POSIX
// placeholder cutover. MAP_FIXED file-over-anon now uses the standard
// FixedRangeGuard + placeholder pipeline; atomicity comes from the kernel
// VAD lock held across MEM_REPLACE_PLACEHOLDER.


/// Decode MAP_HUGE_* size encoding from mmap flags into the PAL's
/// large-page kind enum. Default (`shift == 21` or unspecified) is the
/// 2 MiB `Large` kind; `MAP_HUGE_1GB` (`shift == 30`) selects `Huge`.
nt_pal::LargePageKind decode_huge_page_kind(int flags) {
  int shift = (flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK;
  return (shift == 30) ? nt_pal::LargePageKind::Huge
                       : nt_pal::LargePageKind::Large;
}

/// `SEC_*` flag corresponding to a `LargePageKind` — used by the
/// publish helpers (`derive_region_flags` reads `SEC_LARGE_PAGES /
/// SEC_64K_PAGES` to set the HUGE_PAGES region flag, so we keep the
/// raw kernel flag flowing alongside the typed kind).
ULONG sec_flags_from_kind(nt_pal::LargePageKind kind) {
  return (kind == nt_pal::LargePageKind::Huge) ? SEC_HUGE_PAGES
                                                : SEC_LARGE_PAGES;
}

/// Anonymous large/huge page allocation (MAP_HUGETLB | MAP_ANONYMOUS).
///
/// Routes through Layer 0 PAL: `nt_pal::reserve_large_pages` for the
/// page-kind-aligned placeholder, `nt_pal::commit_replace_large` for
/// the SEC_LARGE_PAGES / SEC_HUGE_PAGES section + view cycle. Produces
/// MEM_MAPPED views for uniform munmap dispatch.
///
/// MAP_HUGE_2MB → 2 MiB (`LargePageKind::Large`, default).
/// MAP_HUGE_1GB → 1 GiB (`LargePageKind::Huge`).
///
/// Returns -EPERM if `SeLockMemoryPrivilege` was not granted at libc
/// init (per design §11.2: no silent downgrade); -ENOTSUP if the
/// kernel reports zero `LargePageMinimum`; -errno otherwise.
///
/// Returns address-as-long on success.
intptr_t alloc_large_pages(void *hint, SIZE_T size, int prot,
                                   int flags) {
  // Privilege gate: design §11.2 forbids silent downgrade to 4 KiB pages.
  if (!nt_pal::large_pages_available())
    return -EPERM;

  nt_pal::LargePageKind kind = decode_huge_page_kind(flags);
  ULONG sec_flags = sec_flags_from_kind(kind);
  SIZE_T page_align = nt_pal::large_page_alignment(kind);
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
        map_large_anon_into_placeholder(ph, page_prot, &section, kind);

    if (LIBC_UNLIKELY(result < 0))
      return result; // ~guard aborts remap guard, ~ph releases placeholder.

    // Commit: sentinel REMAPPING -> LIVE with the new mapping data. The
    // pool takes ownership of `section` on success.
    if (!publish_view_commit(hint, reinterpret_cast<void *>(result), rounded,
                             section, /*file=*/nullptr, LARGE_INTEGER{},
                             page_prot, /*mmap_flags=*/0, sec_flags)) {
      nt_pal::unmap_view_preserve(reinterpret_cast<void *>(result));
      if (section)
        nt_pal::close_section(section);
      return -ENOMEM; // ~guard discards REMAPPING slot.
    }
    guard.mark_committed();
    return result;
  }

  // Non-fixed: placeholder at large-page alignment, then section + map.
  void *hint_base = (hint && windows::is_alloc_aligned(hint)) ? hint : nullptr;
  void *p = nt_pal::reserve_large_pages(hint_base, rounded, kind);
  if (!p && hint_base)
    p = nt_pal::reserve_large_pages(nullptr, rounded, kind);
  if (LIBC_UNLIKELY(!p))
    return -ENOMEM;
  PlaceholderRange ph = PlaceholderRange::from_raw(p, rounded);

  HANDLE section = nullptr;
  intptr_t result =
      map_large_anon_into_placeholder(ph, page_prot, &section, kind);
  if (LIBC_UNLIKELY(result < 0))
    return result; // ~ph releases the placeholder.

  if (!publish_view_register(reinterpret_cast<void *>(result), rounded, section,
                             /*file=*/nullptr, LARGE_INTEGER{}, page_prot,
                             /*mmap_flags=*/0, sec_flags)) {
    nt_pal::unmap_view_preserve(reinterpret_cast<void *>(result));
    nt_pal::free_placeholder(reinterpret_cast<void *>(result));
    if (section)
      nt_pal::close_section(section);
    return -ENOMEM;
  }
  return result;
}

/// File-backed large/huge page allocation (MAP_HUGETLB without MAP_ANONYMOUS).
///
/// Placeholder reservation goes through `nt_pal::reserve_large_pages`
/// (page-kind alignment baked in); the section + map cycle goes through
/// `map_file_into_placeholder` with `SEC_LARGE_PAGES` / `SEC_HUGE_PAGES`
/// passed via the standard `sec_flags` parameter — the file path's
/// fd-table caching, dup-on-cache-hit, and SEC_64K_PAGES fallback
/// orchestration are all reused unchanged.
///
/// MAP_HUGE_2MB → 2 MiB (`LargePageKind::Large`, default).
/// MAP_HUGE_1GB → 1 GiB (`LargePageKind::Huge`).
///
/// Returns -EPERM when `SeLockMemoryPrivilege` was not granted at libc
/// init (per design §11.2: no silent downgrade); the section handle is
/// kept alive and registered for consistent munmap and NUMA remap.
///
/// Returns address-as-long on success.
intptr_t alloc_file_large_pages(void *hint, SIZE_T size, int prot,
                                         int flags, HANDLE file_handle,
                                         off_t offset, int open_flags,
                                         int fd) {
  // Privilege gate: design §11.2 forbids silent downgrade to 4 KiB pages.
  if (!nt_pal::large_pages_available())
    return -EPERM;

  nt_pal::LargePageKind kind = decode_huge_page_kind(flags);
  ULONG sec_flags = sec_flags_from_kind(kind);
  SIZE_T page_align = nt_pal::large_page_alignment(kind);
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

    HANDLE dup_file = nullptr;
    if (file_handle &&
        NT_ERROR(::NtDuplicateObject(NtCurrentProcess(), file_handle,
                                     NtCurrentProcess(), &dup_file, 0, 0,
                                     DUPLICATE_SAME_ACCESS))) {
      nt_pal::unmap_view_preserve(reinterpret_cast<void *>(view));
      if (out_section)
        nt_pal::close_section(out_section);
      return -ENOMEM;
    }
    if (!publish_view_commit(hint, reinterpret_cast<void *>(view), rounded,
                             out_section, dup_file, section_offset, out_prot,
                             flags, sec_flags)) {
      nt_pal::unmap_view_preserve(reinterpret_cast<void *>(view));
      if (out_section)
        nt_pal::close_section(out_section);
      if (dup_file)
        ::NtClose(dup_file);
      return -ENOMEM; // ~guard discards REMAPPING slot.
    }
    guard.mark_committed();
    return view;
  }

  // Non-fixed: large-page-aligned placeholder via PAL.
  void *hint_base = (hint && windows::is_alloc_aligned(hint)) ? hint : nullptr;
  void *p = nt_pal::reserve_large_pages(hint_base, rounded, kind);
  if (!p && hint_base)
    p = nt_pal::reserve_large_pages(nullptr, rounded, kind);
  if (LIBC_UNLIKELY(!p))
    return -ENOMEM;
  PlaceholderRange ph = PlaceholderRange::from_raw(p, rounded);

  HANDLE out_section = nullptr;
  DWORD out_prot = 0;
  intptr_t view = map_file_into_placeholder(ph, file_handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot, sec_flags);
  if (LIBC_UNLIKELY(view < 0))
    return view; // ~ph releases the placeholder.

  // Guards ensure cleanup on any failure after the view is mapped.
  auto section_guard = cpp::make_scope_guard([&] {
    if (out_section)
      nt_pal::close_section(out_section);
  });
  auto view_guard = cpp::make_scope_guard([&] {
    nt_pal::unmap_view(reinterpret_cast<void *>(view));
  });

  // Dup the borrowed file_handle (fd_table retains its copy); transfer
  // the freshly-created out_section directly (no dup + close cycle).
  HANDLE dup_file = nullptr;
  if (file_handle) {
    NTSTATUS dup_st = ::NtDuplicateObject(
        NtCurrentProcess(), file_handle, NtCurrentProcess(), &dup_file, 0, 0,
        DUPLICATE_SAME_ACCESS);
    if (NT_ERROR(dup_st))
      return -ENOMEM; // ~section_guard closes section, ~view_guard unmaps.
  }

  if (!publish_view_register(reinterpret_cast<void *>(view), rounded,
                             out_section, dup_file, section_offset, out_prot,
                             flags, sec_flags)) {
    // ~section_guard / ~view_guard restore via early return.
    return -ENOMEM;
  }
  section_guard.dismiss();
  view_guard.dismiss();
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
  //
  // Dup on cache hit: the cache retains the master handle; callers always
  // receive an owned handle so the RegionPool can take ownership without
  // aliasing the cache (which would dangle on unmap).
  HANDLE section = nullptr;
  bool section_owned = true;
  if (fd >= 0) {
    internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
    if (ofd) {
      HANDLE cached =
          ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE);
      if (cached) {
        HANDLE dup = nullptr;
        if (NT_SUCCESS(::NtDuplicateObject(process, cached, process, &dup, 0, 0,
                                            DUPLICATE_SAME_ACCESS)))
          section = dup;
      }
    }
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

    NTSTATUS status = nt_pal::create_section_file(
        file_handle, /*max_size=*/0, section_prot, section_access, &section,
        effective_sec_flags);
    if (NT_ERROR(status)) {
      // SEC_64K_PAGES may fail on older builds or certain file types.
      // Fall back to plain SEC_COMMIT.
      if (effective_sec_flags != sec_flags) {
        status = nt_pal::create_section_file(file_handle, /*max_size=*/0,
                                             section_prot, section_access,
                                             &section, sec_flags);
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
            nt_pal::close_section(dup); // Lost race — winner's handle already stored.
        }
      }
    }
  }

  // Determine the view's page protection.
  bool is_private = (flags & MAP_PRIVATE) != 0;
  DWORD view_prot = is_private ? windows::prot_to_page_flags_cow(prot)
                               : windows::prot_to_page_flags(prot);

  // Section offset must be page-aligned (caller's responsibility). With
  // MEM_REPLACE_PLACEHOLDER the kernel constrains the view to the
  // placeholder's exact base / size; no kernel rewrite of those values
  // is observable.
  void *placeholder_base = ph.base();
  NTSTATUS map_st = nt_pal::map_section_replace(section, placeholder_base,
                                                ph.size(), section_offset,
                                                view_prot);

  if (NT_ERROR(map_st)) {
    if (section_owned)
      nt_pal::close_section(section);
    return windows_util::ntstatus_to_kerr(map_st);
  }

  (void)ph.consume(); // Placeholder consumed by the section view.

  if (out_section)
    *out_section = section;
  else if (section_owned)
    nt_pal::close_section(section);

  if (out_view_prot)
    *out_view_prot = view_prot;

  return cpp::bit_cast<intptr_t>(placeholder_base);
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

  // File-backed: section view. MAP_PRIVATE uses PAGE_WRITECOPY (kernel CoW),
  // MAP_SHARED uses the requested prot — both select inside
  // map_file_into_placeholder based on (flags & MAP_PRIVATE).
  HANDLE out_section = nullptr;
  DWORD out_prot = 0;
  intptr_t view = map_file_into_placeholder(ph, file_handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot);
  if (LIBC_UNLIKELY(view < 0))
    return view; // ~guard aborts, ~ph releases.

  // Commit: sentinel REMAPPING -> LIVE with the new file mapping data.
  // Pool takes ownership of section + a duped file handle.
  void *view_base = reinterpret_cast<void *>(view);
  HANDLE dup_file = nullptr;
  if (file_handle &&
      NT_ERROR(::NtDuplicateObject(NtCurrentProcess(), file_handle,
                                   NtCurrentProcess(), &dup_file, 0, 0,
                                   DUPLICATE_SAME_ACCESS))) {
    nt_pal::unmap_view_preserve(view_base);
    if (out_section)
      nt_pal::close_section(out_section);
    return -ENOMEM;
  }
  if (!publish_view_commit(placeholder_addr, view_base, view_size, out_section,
                           dup_file, section_offset, out_prot, flags,
                           SEC_COMMIT)) {
    nt_pal::unmap_view_preserve(view_base);
    if (out_section)
      nt_pal::close_section(out_section);
    if (dup_file)
      ::NtClose(dup_file);
    return -ENOMEM; // ~guard discards REMAPPING slot.
  }
  guard.mark_committed();

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

  // File-backed: always section view. MAP_PRIVATE maps PAGE_WRITECOPY (kernel
  // CoW); MAP_SHARED maps with the requested prot. Selected inside
  // map_file_into_placeholder.
  HANDLE out_section = nullptr;
  DWORD out_prot = 0;
  intptr_t view = map_file_into_placeholder(ph, file_handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot);
  if (LIBC_UNLIKELY(view < 0))
    return view; // ~ph releases the placeholder.

  // Guards ensure cleanup on any failure after the view is mapped.
  auto section_guard = cpp::make_scope_guard([&] {
    if (out_section)
      nt_pal::close_section(out_section);
  });
  auto view_guard = cpp::make_scope_guard([&] {
    nt_pal::unmap_view(reinterpret_cast<void *>(view));
  });

  void *view_base = reinterpret_cast<void *>(view);
  HANDLE dup_file = nullptr;
  if (file_handle) {
    NTSTATUS dup_st = ::NtDuplicateObject(
        NtCurrentProcess(), file_handle, NtCurrentProcess(), &dup_file, 0, 0,
        DUPLICATE_SAME_ACCESS);
    if (NT_ERROR(dup_st))
      return -ENOMEM; // ~section_guard closes section, ~view_guard unmaps.
  }

  if (!publish_view_register(view_base, view_size, out_section, dup_file,
                             section_offset, out_prot, flags, SEC_COMMIT)) {
    return -ENOMEM; // ~section_guard / ~view_guard clean up.
  }
  section_guard.dismiss();
  view_guard.dismiss();

  return view + delta;
}

/// Create a large/huge-page section and map it into a placeholder.
///
/// Thin wrapper over `nt_pal::commit_replace_large` — the placeholder is
/// consumed on success, untouched on failure. Two callers
/// (`alloc_large_pages` MAP_FIXED and non-fixed paths) keeps the helper
/// useful even though the body is now small.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t map_large_anon_into_placeholder(PlaceholderRange &ph, DWORD prot,
                                         HANDLE *out_section,
                                         nt_pal::LargePageKind kind) {
  void *placeholder_base = ph.base();
  HANDLE section = nullptr;
  NTSTATUS st = nt_pal::commit_replace_large(placeholder_base, ph.size(), prot,
                                             kind, &section);
  if (NT_ERROR(st)) {
    return (st == STATUS_PRIVILEGE_NOT_HELD || st == STATUS_ACCESS_DENIED)
               ? -static_cast<intptr_t>(EPERM)
               : windows_util::ntstatus_to_kerr(st);
  }

  (void)ph.consume(); // Placeholder consumed by the section view.

  if (out_section)
    *out_section = section;
  else
    nt_pal::close_section(section);

  return cpp::bit_cast<intptr_t>(placeholder_base);
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
/// Placeholder-only: every anon mmap is placeholder-origin so MAP_FIXED
/// file-over-anon can atomically REPLACE_PLACEHOLDER against both in-process
/// and cross-process VA mutators (the kernel holds the VAD lock across the
/// replacement). Two syscalls (reserve + commit) for standard commit; one
/// (reserve only) for PROT_NONE; one (reserve-replace) for MAP_NORESERVE.
///
/// Returns address-as-long on success, -errno on failure.
intptr_t alloc_hint_placeholder(void *hint, SIZE_T size, int prot,
                                         int flags) {
  PlaceholderRange ph;

  if (hint && windows::is_page_aligned(hint)) {
    if (windows::is_alloc_aligned(hint)) {
      ph = PlaceholderRange::reserve(size, hint);
    } else {
      // Page-aligned but not 64KB-aligned: reserve oversized at the
      // 64KB-aligned floor, then split at the exact hint address.
      uintptr_t hint_addr = reinterpret_cast<uintptr_t>(hint);
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
          }
        }
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

  SIZE_T ph_size = ph.size();
  return alloc_fixed_free_commit(ph.consume().base, ph_size, prot, flags);
}

/// Commit into an already-claimed placeholder (anonymous).
/// Used by NOREPLACE (placeholder claimed atomically) and alloc_fixed_free.
intptr_t alloc_fixed_free_commit(void *placeholder, SIZE_T size,
                                         int prot, int flags) {
  // Adopt ownership — destructor releases on error paths.
  PlaceholderRange ph = PlaceholderRange::from_raw(placeholder, size);

  if (prot == PROT_NONE) {
    void *base = ph.base();
    SIZE_T sz = ph.size();
    (void)ph.consume();
    if (!windows::publish_anon_placeholder_protnone(base, sz)) {
      nt_pal::free_placeholder(base);
      return -ENOMEM;
    }
    return cpp::bit_cast<intptr_t>(base);
  }

  if (flags & MAP_NORESERVE) {
    SIZE_T sz = ph.size();
    intptr_t result = map_anon_reserve_into_placeholder(ph, prot);
    if (LIBC_UNLIKELY(result < 0))
      return result; // ~ph releases the placeholder.
    void *base = reinterpret_cast<void *>(result);
    DWORD page_prot = windows::prot_to_page_flags(prot);
    if (!windows::publish_anon_placeholder_noreserve(base, sz, page_prot)) {
      nt_pal::free_placeholder(base);
      return -ENOMEM;
    }
    return result;
  }

  // Standard private commit: placeholder replaced with MEM_PRIVATE committed
  // memory. Kernel state is identical to the retired ANON_ONESHOT — publish
  // under ANON_PLACEHOLDER with region_flag::COMMITTED so partial-munmap
  // dispatch uses the single-syscall MEM_DECOMMIT recipe.
  SIZE_T sz = ph.size();
  intptr_t result = map_anon_private_into_placeholder(ph, prot);
  if (LIBC_UNLIKELY(result < 0))
    return result; // ~ph releases the placeholder.
  void *base = reinterpret_cast<void *>(result);
  DWORD page_prot = windows::prot_to_page_flags(prot);
  if (!windows::publish_anon_committed(base, sz, page_prot)) {
    nt_pal::free_va(base);
    return -ENOMEM;
  }
  return result;
}

/// MAP_FIXED into a MEM_FREE range.
///
/// Placeholder-only: reserve a placeholder at the exact address, then commit
/// via alloc_fixed_free_commit. Sub-granularity hints use oversized+split.
/// MAP_FIXED requires the exact address — no fallback to system-chosen.
/// Returns -errno on failure (caller retries with fresh classification).
intptr_t alloc_fixed_free(void *addr, SIZE_T size, int prot,
                                  int flags) {
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
        }
      }
    }
  }

  if (!ph)
    return -ENOMEM; // Range occupied or VA exhausted — caller reclassifies.

  return alloc_fixed_free_commit(ph.consume().base, size, prot, flags);
}

/// Lock-free MAP_FIXED dispatcher for anonymous mappings.
///
/// Shape-driven path: snapshot the slot covering `addr` and route by
/// RegionShape. For tracked regions this avoids the per-attempt MBI
/// classify pair (initial probe + post-failure stale check). MBI is
/// consulted only on table miss (untracked memory).
///
/// Retry loop: if a sub-function fails and the table state at `addr`
/// changed (concurrent mutation), re-snapshots and re-dispatches.
///
/// Returns address-as-long on success, -errno on failure.
// Classify-and-dispatch the MAP_FIXED-anon overwrite path.
//
// Ownership policy (Gap #4 fix): the mapping table is the SOLE source of
// truth for "memory we own." Any VA we could potentially destroy must be
// represented by a LIVE / PLACEHOLDER / FOREIGN slot before we touch it.
//   * snapshot succeeds → dispatch on shape.
//   * snapshot misses + MBI=FREE → safe to allocate.
//   * snapshot misses + MBI=non-FREE → STAMP FOREIGN and refuse. We never
//     auto-take ownership of memory we cannot prove is ours; doing so would
//     happily unmap NT loader DLL sections, CRT heap arenas, thread pool
//     reservations, KUSER_SHARED_DATA, or another process's VirtualAlloc2.
//     The legacy "MEM_PRIVATE without tracking slot = legacy untracked" path
//     was that bug; it is gone.
//
// Concurrency: snapshot internally waits on WRITING via the slot's version
// futex, so a concurrent T2 publishing at the same addr cannot race past
// us — we either see T2's pre-publish state (KEY_FREE) and proceed, or
// observe T2's WRITING window and block until it closes. A retry loop
// covers the "T2 published after our snapshot but before our NT call"
// window: our NT call fails CONFLICTING_ADDRESSES, the second snapshot
// now sees T2's published state, and we re-dispatch.
intptr_t alloc_fixed_anon(void *addr, SIZE_T size, int prot, int flags) {
  constexpr int MAX_CLASSIFY_RETRIES = 4;

  char *aligned_addr = reinterpret_cast<char *>(
      windows::align_down_to_granularity(
          reinterpret_cast<uintptr_t>(addr)));

  for (int attempt = 0; attempt < MAX_CLASSIFY_RETRIES; ++attempt) {
    intptr_t result = -ENOMEM;

    // Snapshot. Authoritative for any VA we own; foreign and free both
    // surface as "no region" below.
    windows::SlotSnapshot snap;
    const bool have_snap =
        windows::g_mapping_table.snapshot(aligned_addr, &snap) &&
        snap.region != nullptr;

    if (have_snap) {
      const windows::memory::RegionShape shape = snap.region->current_shape();
      switch (shape) {
      case windows::memory::RegionShape::ANON_PLACEHOLDER: {
        char *region_base = static_cast<char *>(snap.view_base);
        char *region_end = region_base + snap.view_size;
        char *target_start = static_cast<char *>(addr);
        char *target_end = target_start + size;
        if (target_start >= region_base && target_end <= region_end)
          result = alloc_fixed_private_placeholder(addr, size, prot);
        else
          result = alloc_fixed_private_multi(addr, size, prot);
        break;
      }
      case windows::memory::RegionShape::FILE_VIEW_MONO:
      case windows::memory::RegionShape::FILE_VIEW_CHUNKED:
      case windows::memory::RegionShape::FILE_VIEW_RESERVE:
      case windows::memory::RegionShape::ANON_RESERVE_SECTION:
        result = alloc_fixed_split_remap(addr, size, prot, flags);
        break;
      case windows::memory::RegionShape::LIBC_INTERNAL:
      case windows::memory::RegionShape::IMAGE_REGION:
      case windows::memory::RegionShape::KERNEL_REGION:
        return -EINVAL; // Tracked non-MAP_FIXED targets.
      case windows::memory::RegionShape::FOREIGN_SENTINEL:
        return -ENOMEM; // Cordoned — POSIX: cannot allocate.
      case windows::memory::RegionShape::NONE:
        // Should not occur for a snap with non-null region.
        return -ENOMEM;
      }
    } else {
      // Table miss. Probe NT once — the only legitimate outcomes are
      // genuinely-FREE VA or memory we don't own.
      MEMORY_BASIC_INFORMATION mbi;
      if (!nt_pal::query_region(addr, mbi))
        return -EINVAL;

      if (mbi.State == MEM_FREE) {
        result = alloc_fixed_free(addr, size, prot, flags);
      } else {
        // Non-FREE + table miss = foreign by definition. Stamp the slot at
        // the requested address with a FOREIGN cordon so subsequent
        // snapshots converge in O(1) without another MBI poll, then refuse
        // the request. Cordoning the requested slot (rather than the full
        // AllocationBase span) keeps the hot path O(1); other addresses in
        // the same foreign region pay one MBI poll on first touch.
        //
        // The stamp can fail when a peer thread has just published a real
        // owned slot here in the table-miss → stamp window. In that case
        // the address is ours after all; loop back and re-snapshot rather
        // than reporting -ENOMEM against memory we own.
        if (!windows::g_mapping_table.register_foreign(
                aligned_addr,
                /*extent=*/static_cast<SIZE_T>(mbi.RegionSize),
                /*region_id=*/0, /*alloc_id=*/0))
          continue;
        return -ENOMEM;
      }
    }

    if (result >= 0)
      return result;

    // Failure leg. Park on the slot's key until any state transition lands
    // — UMWAIT/MWAITX hardware monitor first, then NtWaitForAlertByThreadId
    // if no wake arrives in the spin budget. publish_slot wakes on the
    // same address, so this is the precise signal: T2 finishing publish,
    // foreign owner releasing, or our own concurrent FOREIGN stamp — any
    // of these unpark us, after which the next snapshot reflects truth.
    windows::g_mapping_table.wait_for_state_change(aligned_addr);
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

intptr_t legacy_mmap_engine(void *addr, size_t size, int prot, int flags,
                            int fd, off_t offset) {
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

  // --- MAP_FIXED_NOREPLACE: atomic VA claim via placeholder ---
  //
  // NtAllocateVirtualMemoryEx with MEM_RESERVE_PLACEHOLDER at the exact
  // address claims the VA atomically against all in-process AND cross-process
  // VA mutators (the kernel holds the VAD lock across placeholder reserve).
  // If the range is occupied the call fails → EEXIST.
  //
  // HUGETLB exception: large-page paths manage their own placeholder
  // lifecycle internally. Fall back to nt_pal::is_range_free (best-effort).
  PlaceholderRange noreplace_ph;
  if (flags & MAP_FIXED_NOREPLACE) {
    if (LIBC_UNLIKELY(!addr || !windows::is_page_aligned(addr)))
      return -EINVAL;

    if (flags & MAP_HUGETLB) {
      if (!nt_pal::is_range_free(addr, rounded_size))
        return -EEXIST;
      flags |= MAP_FIXED;
    } else {
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
            }
          }
        }
      }

      if (!claim)
        return -EEXIST;
      noreplace_ph = cpp::move(claim);
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
  // All anon allocations are placeholder-origin. MAP_SHARED|MAP_ANONYMOUS
  // treated as private on Windows (no fork).
  if (flags & MAP_ANONYMOUS) {
    intptr_t result;

    if (noreplace_ph) {
      // NOREPLACE: atomic claim already succeeded. Commit into it.
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
      nt_pal::prefetch_committed(reinterpret_cast<void *>(result),
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

      // File-backed: always section view. MAP_PRIVATE maps PAGE_WRITECOPY
      // (kernel CoW); MAP_SHARED maps with the requested prot.
      HANDLE out_section = nullptr;
      DWORD out_prot = 0;
      result = map_file_into_placeholder(noreplace_ph, handle,
                                         section_offset, prot, flags,
                                         open_flags, fd, &out_section,
                                         &out_prot);
      if (LIBC_UNLIKELY(result < 0))
        return result; // ~noreplace_ph releases

      // Guards ensure cleanup on any failure after the view is mapped.
      auto section_guard = cpp::make_scope_guard([&] {
        if (out_section)
          nt_pal::close_section(out_section);
      });
      auto view_guard = cpp::make_scope_guard([&] {
        nt_pal::unmap_view(reinterpret_cast<void *>(result));
      });

      HANDLE dup_file = nullptr;
      if (handle) {
        NTSTATUS dup_st = ::NtDuplicateObject(
            NtCurrentProcess(), handle, NtCurrentProcess(), &dup_file, 0, 0,
            DUPLICATE_SAME_ACCESS);
        if (NT_ERROR(dup_st))
          return -ENOMEM; // ~section_guard + ~view_guard clean up.
      }

      if (!publish_view_register(reinterpret_cast<void *>(result),
                                 rounded_size, out_section, dup_file,
                                 section_offset, out_prot, flags,
                                 SEC_COMMIT))
        return -ENOMEM; // ~section_guard / ~view_guard / ~noreplace_ph
      section_guard.dismiss();
      view_guard.dismiss();
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
      nt_pal::prefetch_committed(reinterpret_cast<void *>(result),
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

LIBC_REGISTER_FORK_REINIT(mmap_lock,
                          ::LIBC_NAMESPACE::internal::kForkPrioMmapLock,
                          &::LIBC_NAMESPACE::internal::mmap_lock_fork_reinit)
