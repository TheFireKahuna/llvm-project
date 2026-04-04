//===-- Anonymous region publish / trim helpers -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared mapping-table publication helpers for the single anonymous shape
// ANON_PLACEHOLDER. Every anon allocation is placeholder-origin; sub-flavor
// is encoded in (slot state, region flags):
//
//   COMMITTED flag → slot LIVE, placeholder replaced with private committed
//                    VA. Partial munmap uses MEM_DECOMMIT (kernel-identical
//                    to the retired ANON_ONESHOT).
//   NORESERVE flag → slot LIVE, placeholder reserve-replaced with MEM_RESERVE
//                    private VA. VEH commits on fault.
//   neither flag  → slot PLACEHOLDER, PROT_NONE bare reservation.
//                    VEH propagates SIGSEGV.
//
// Slot bookkeeping for partial unmap that splits a kernel allocation
// lives in `trim_anon_slot_for_hole`. That helper is shared by:
//
//   * `vm_protect`'s ANON_PLACEHOLDER mid-region partial unmap
//   * `mmap_engine`'s `alloc_fixed_private_multi` slot-aware teardown
//
// Both call sites must hold MmapLock writer when invoking the trim
// helper — the original slot is extracted and surviving head/tail
// fragments are republished under fresh region descriptors. After a
// partial release, each surviving kernel allocation is its own
// AllocationBase, so each survivor needs its own RegionDesc; the plan
// reserves CHUNKED-style multi-slot regions for section-backed shapes
// only.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ANON_REGION_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ANON_REGION_OPS_H

#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/region_desc.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

namespace anon_ops_detail {

// Acquire a fresh ANON_* region descriptor wrapped in a RegionTicket. The
// ticket auto-releases the region when dropped without `commit()` — used by
// every prepare-then-commit publish path so a publish-side failure cannot
// leak a half-acquired region.
//
// `size == 0` returns an empty (non-valid) ticket — caller skips the
// associated fragment. The check lives here so trim_anon_slot_for_hole can
// blindly reserve head + tail without per-call zero-skip plumbing.
[[nodiscard]] LIBC_INLINE memory::RegionTicket
reserve_anon_region(void *base, SIZE_T size,
                    memory::RegionShape shape, uint16_t flags) {
  if (size == 0)
    return memory::RegionTicket{};
  memory::AcquireSpec spec{};
  spec.section_handle = nullptr;
  spec.file_handle = nullptr;
  spec.section_offset = LARGE_INTEGER{};
  spec.shape = shape;
  spec.flags = flags;
  spec.first_slot_key = reinterpret_cast<uintptr_t>(base) >> 16;
  spec.last_slot_key = spec.first_slot_key + (size >> 16);
  return memory::g_region_pool.reserve(spec);
}

// Bind a pre-reserved ticket onto a slot. `placeholder_state` selects
// register_placeholder vs register_mapping. On success, commits the ticket
// (caller must NOT release manually). On register failure (CAS conflict
// despite our locking discipline), the ticket's RAII release closes the
// region; caller observes the failure via the bool return.
[[nodiscard]] LIBC_INLINE bool
commit_survivor_fragment(memory::RegionTicket &ticket, void *base, SIZE_T size,
                         DWORD view_prot, bool placeholder_state) {
  if (!ticket)
    return true; // Empty fragment — no-op.
  bool published =
      placeholder_state
          ? g_mapping_table.register_placeholder(base, size, ticket.region_id(),
                                                 ticket.alloc_id())
          : g_mapping_table.register_mapping(base, size, ticket.region_id(),
                                             ticket.alloc_id(), view_prot,
                                             /*flags=*/0);
  if (!published)
    return false; // ~ticket releases the region.
  ticket.commit();
  return true;
}

} // namespace anon_ops_detail

// =====================================================================
// Publish helpers
// =====================================================================

/// Publish a standard-committed anonymous placeholder-origin allocation.
/// Slot is LIVE; region_flag::COMMITTED drives the single-syscall
/// MEM_DECOMMIT partial-munmap recipe (kernel-identical to the retired
/// ANON_ONESHOT shape).
[[nodiscard]] LIBC_INLINE bool publish_anon_committed(void *base, SIZE_T size,
                                                     DWORD view_prot) {
  memory::RegionTicket t = anon_ops_detail::reserve_anon_region(
      base, size, memory::RegionShape::ANON_PLACEHOLDER,
      memory::region_flag::COMMITTED);
  if (!t)
    return false;
  return anon_ops_detail::commit_survivor_fragment(
      t, base, size, view_prot, /*placeholder_state=*/false);
}

/// Publish a PROT_NONE anonymous reservation. Slot is in PLACEHOLDER
/// state so the VEH master handler propagates SIGSEGV instead of
/// engaging the demand-commit path.
[[nodiscard]] LIBC_INLINE bool
publish_anon_placeholder_protnone(void *base, SIZE_T size) {
  memory::RegionTicket t = anon_ops_detail::reserve_anon_region(
      base, size, memory::RegionShape::ANON_PLACEHOLDER, /*flags=*/0);
  if (!t)
    return false;
  return anon_ops_detail::commit_survivor_fragment(
      t, base, size, /*view_prot=*/0, /*placeholder_state=*/true);
}

/// Publish a MAP_NORESERVE anonymous reservation. Slot is LIVE; the
/// region_flag::NORESERVE bit drives the VEH demand-commit path.
[[nodiscard]] LIBC_INLINE bool
publish_anon_placeholder_noreserve(void *base, SIZE_T size, DWORD view_prot) {
  memory::RegionTicket t = anon_ops_detail::reserve_anon_region(
      base, size, memory::RegionShape::ANON_PLACEHOLDER,
      memory::region_flag::NORESERVE);
  if (!t)
    return false;
  return anon_ops_detail::commit_survivor_fragment(
      t, base, size, view_prot, /*placeholder_state=*/false);
}

// =====================================================================
// Trim / split helper
// =====================================================================

/// Hole [hole_start, hole_end) has been (or is about to be) punched out
/// of the anon region whose slot is at `slot_base`. Adjust the table to
/// match: extract the original slot, and re-publish surviving head
/// (`[slot_base, hole_start)`) and tail (`[hole_end, slot_end)`)
/// fragments under fresh region descriptors.
///
/// Lock contract — the caller must serialize against any other thread
/// mutating slots inside [slot_base, slot_base + slot_size). Two regimes
/// satisfy that contract:
///
///   * MmapLock shared (the ordinary munmap / mprotect path). POSIX
///     forbids concurrent munmap of the same address, and the table-level
///     CAS in extract() / register_*() handles benign races against
///     concurrent reads.
///   * MmapLock writer — required when this trim is part of a multi-
///     AllocationBase teardown (alloc_fixed_private_multi) where the
///     caller is mutating several adjacent regions in one logical step
///     and needs the full range frozen for the duration.
///
/// Returns true on success or no-op. Returns false **only** when survivor
/// region descriptors cannot be reserved up front — in that case the
/// original slot is left untouched (no kernel state modified, no untracked
/// VA window). This is the prepare-then-commit transaction discipline:
///   1. Snapshot original slot, derive survivor extents and shape/flags.
///   2. Pre-reserve RegionTickets for the head and tail survivors.
///      Reservation failure (pool OOM) bails before any destructive op.
///   3. Extract the original slot. From this point all work is in-memory
///      slot transitions — register_placeholder / register_mapping on a
///      freshly-extracted address cannot fail under our locking discipline
///      (radix expansion acquires a substrate sub-slot; the WRITING-bit CAS spins
///      to convergence on transient contention; no slot conflict is
///      possible against a just-freed address held under the caller's
///      lock).
///   4. Commit each ticket onto its survivor slot. Failure here would
///      indicate a real CAS conflict that violates the lock contract —
///      treated as a programming bug; ticket RAII still releases the
///      region so we never leak.
[[nodiscard]] LIBC_INLINE bool
trim_anon_slot_for_hole(void *slot_base, uintptr_t hole_start,
                        uintptr_t hole_end) {
  SlotSnapshot snap;
  if (!g_mapping_table.snapshot(slot_base, &snap) || snap.region == nullptr)
    return true; // Nothing to trim — already gone.

  uintptr_t slot_lo = reinterpret_cast<uintptr_t>(snap.view_base);
  uintptr_t slot_hi = slot_lo + snap.view_size;
  if (hole_start < slot_lo)
    hole_start = slot_lo;
  if (hole_end > slot_hi)
    hole_end = slot_hi;
  if (hole_start >= hole_end)
    return true; // Empty intersection.

  const memory::RegionShape shape = snap.region->current_shape();
  if (shape != memory::RegionShape::ANON_PLACEHOLDER)
    return true; // Non-anon shapes manage their own bookkeeping.

  const bool nor_flag =
      snap.region->has_flag(memory::region_flag::NORESERVE);
  const bool committed_flag =
      snap.region->has_flag(memory::region_flag::COMMITTED);
  // PROT_NONE is the "neither flag set" state — slot lives as PLACEHOLDER.
  const bool placeholder_state = !nor_flag && !committed_flag;
  uint16_t region_flags = 0;
  if (nor_flag)
    region_flags |= memory::region_flag::NORESERVE;
  if (committed_flag)
    region_flags |= memory::region_flag::COMMITTED;
  const DWORD view_prot = snap.view_prot;

  void *head_base = reinterpret_cast<void *>(slot_lo);
  const SIZE_T head_size =
      (hole_start > slot_lo) ? static_cast<SIZE_T>(hole_start - slot_lo) : 0;
  void *tail_base = reinterpret_cast<void *>(hole_end);
  const SIZE_T tail_size =
      (hole_end < slot_hi) ? static_cast<SIZE_T>(slot_hi - hole_end) : 0;

  // (2) Pre-reserve survivor descriptors. RAII drop releases on early exit.
  memory::RegionTicket head_ticket =
      anon_ops_detail::reserve_anon_region(head_base, head_size, shape,
                                           region_flags);
  if (head_size > 0 && !head_ticket)
    return false; // Pool OOM — original slot intact.

  memory::RegionTicket tail_ticket =
      anon_ops_detail::reserve_anon_region(tail_base, tail_size, shape,
                                           region_flags);
  if (tail_size > 0 && !tail_ticket)
    return false; // ~head_ticket releases head; original slot intact.

  // (3) Extract the original slot — drops its region ref. From here on
  // tickets must either commit or release via RAII; the original is gone.
  MappingEntry entry;
  if (!g_mapping_table.extract(snap.view_base, &entry))
    return true; // Concurrent extract — original already gone; ~tickets release.
  if (entry.region_id != memory::RegionPool::NONE)
    memory::g_region_pool.release(entry.region_id);

  // (4) Commit survivors. Failure here is a lock-contract violation; the
  // boolean propagates so tests in stress paths can fail loudly. Ticket
  // RAII still releases on the failure leg, so no region ref leaks.
  bool ok = true;
  if (!anon_ops_detail::commit_survivor_fragment(head_ticket, head_base,
                                                 head_size, view_prot,
                                                 placeholder_state))
    ok = false;
  if (!anon_ops_detail::commit_survivor_fragment(tail_ticket, tail_base,
                                                 tail_size, view_prot,
                                                 placeholder_state))
    ok = false;

  return ok;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ANON_REGION_OPS_H
