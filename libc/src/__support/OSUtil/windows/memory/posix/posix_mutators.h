//===- posix_mutators.h - DescMutator callbacks for POSIX ops ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `va_tracker::DescMutator` callbacks for every POSIX op that mutates a
/// `RegionDesc` in place via `va_tracker::mutate`. Each mutator is a
/// free function with a fixed pointer signature; callers pass a
/// matching context struct from this header in `void *ctx`.
///
/// The mutator runs inside the substrate's `nt_pal` phase on a fresh
/// clone of the source descriptor (per `va_tracker.h::mutate`), so each
/// callback writes to `new_desc` directly without atomicity concerns —
/// the clone is not yet visible to any reader, and the substrate's
/// Swap-CAS provides the publish-side `release` fence. Existing flag
/// bits on the clone are preserved by the OR / AND-NOT pattern in every
/// mutator; that preservation is the cross-cutting invariant the
/// substrate's mutator-merge logic relies on.
///
/// Twelve mutators, each ≤ 10 lines. No dispatch table; no generic
/// mutator. The repetition is the point: the substrate gets one fixed
/// pointer per intent, and a future reader can see what each one
/// touches without indirection.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_MUTATORS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_MUTATORS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory_posix {

//===----------------------------------------------------------------------===//
// Context structs.
//===----------------------------------------------------------------------===//

/// Context for `brk_extend_mutator`. Tracks the new brk cursor and the
/// placeholder window the cursor moves through. The substrate fires
/// `commit_replace` over `[new_cursor - old_cursor)` after the mutator
/// returns, when `prot_change` on the enclosing `mutate` call is
/// non-zero.
struct BrkExtendCtx {
  /// New end-of-data-segment cursor (page-aligned). Must lie within the
  /// `[placeholder_base, placeholder_base + placeholder_size)` window
  /// the originating `brk_meta` reserved.
  void *new_cursor;
};

/// Context for `numa_rebind_mutator`. Carries the new policy mode plus
/// the active-node mask. The fault handler's NUMA-rotating commit path
/// reads `flags & NUMA_INTERLEAVE` and the desc's `numa_interleave_mask`
/// to drive per-page commit-from-node selection.
struct NumaRebindCtx {
  /// `MPOL_INTERLEAVE` enables the bit; other modes clear it.
  /// `MPOL_DEFAULT` clears the mask and bit together.
  bool interleave;
  /// Active-node mask. Bit N set ⇔ node N is in the policy's set.
  uint32_t nodemask;
};

//===----------------------------------------------------------------------===//
// Mutator declarations.
//
// Every mutator matches `va_tracker::DescMutator`:
//   void (*)(RegionDesc *new_desc, void *ctx)
//===----------------------------------------------------------------------===//

/// `mlock` / `munlock` immediate-lock case — no per-desc state change.
///
/// Immediate locking is tracked entirely by the kernel's per-page lock
/// counter; the desc carries no flag for it. The mutator stays as a
/// no-op so the substrate's `mutate(...)` envelope can still serve as
/// the publish-and-protect path when an op needs `prot_change` on the
/// same range, and as a typed entry point even when no field changes.
///
/// `mlock` itself does not currently route through this mutator (it
/// calls `nt_pal::lock_range` directly inside a kernel-VAD walk —
/// every page in the range gets locked, including foreign / image VA
/// the va_tracker does not see). Kept callable in case a future shape
/// shift wants to attach diagnostics or telemetry to the lock window.
void lock_mutator(::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc,
                  void *ctx);

/// `mlock2(MLOCK_ONFAULT)` — set `region_flag::MLOCK_ONFAULT`.
///
/// The fault handler in `mem_fault_handler.cpp` reads the bit on every
/// resolve and locks the faulting page on hit. The bit replaces the
/// legacy global `OnfaultState` table: per-desc storage lives in the
/// existing `flags` atomic; lookup is a flag-mask off the already-
/// pinned desc; teardown is the symmetric `lock_clear_mutator`.
///
/// PAGE_GUARD is applied to the range as a side effect of the
/// `mlock2` entry (not by this mutator — the protection write needs
/// the kernel-VAD walk, and the substrate's `mutate` already
/// serialises the publish window for us). On first touch the kernel
/// raises `STATUS_GUARD_PAGE_VIOLATION`, the fault handler resolves
/// the desc, sees the flag, and locks.
void lock_set_onfault_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `munlock` — clear `region_flag::MLOCK_ONFAULT`.
///
/// Pairs with `lock_set_onfault_mutator`. PAGE_GUARD residue on
/// already-armed pages is harmless: the kernel auto-clears PAGE_GUARD
/// on the trip through the fault handler, and with the flag now
/// clear the handler returns `EXCEPTION_CONTINUE_SEARCH` rather than
/// re-locking.
void lock_clear_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `brk` / `sbrk` cursor extension. `ctx` is a `BrkExtendCtx *`. The
/// mutator runs as the metadata-update step; the substrate handles the
/// kernel-side `commit_replace` on the new tail.
void brk_extend_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `mbind(MPOL_INTERLEAVE)` / `mbind(MPOL_DEFAULT)`. `ctx` is a
/// `NumaRebindCtx *`. Toggles `region_flag::NUMA_INTERLEAVE` and writes
/// the active-node mask. The fault handler's rotating commit path picks
/// up the new mask on the next demand-commit.
void numa_rebind_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_DONTDUMP` — set `region_flag::DUMP_EXCLUDE`. The PEB WER
/// gather-list write is the durable side effect, performed by the
/// caller after the mutator returns.
void dump_set_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_DODUMP` — clear `region_flag::DUMP_EXCLUDE`.
void dump_clear_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_GUARD_INSTALL` — set `region_flag::PROT_GUARD`. The caller in
/// `madvise_guard.cpp` is responsible for the initial
/// `NtProtect(PAGE_NOACCESS)` call after the mutator publishes the new
/// desc; this mutator only updates the flag bit so the fault handler
/// recognises guarded pages on the next access.
void guard_set_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_GUARD_REMOVE` — clear `region_flag::PROT_GUARD`. The caller
/// restores the baseline page protection after the mutator publishes;
/// this mutator only clears the flag.
void guard_clear_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_DONTFORK` — set `region_flag::DONTFORK`. Consumed by
/// `va_tracker_fork_reinit` to skip the region in the child.
void fork_set_dontfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_DOFORK` — clear `region_flag::DONTFORK`.
void fork_clear_dontfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_WIPEONFORK` — set `region_flag::WIPEONFORK`. Consumed by
/// `va_tracker_fork_reinit` to zero the region in the child after
/// replay.
void fork_set_wipeonfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

/// `MADV_KEEPONFORK` — clear `region_flag::WIPEONFORK`.
void fork_clear_wipeonfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

} // namespace memory_posix
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_MUTATORS_H
