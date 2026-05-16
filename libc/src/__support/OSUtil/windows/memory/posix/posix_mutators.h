//===- posix_mutators.h - DescMutator callbacks for POSIX ops ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `va_tracker::DescMutator` callbacks for the POSIX mutating ops
// (mlock / mbind / madvise / brk). Free functions only — no lambdas,
// no captures, no `std::function`: every callback is a stable C symbol
// so `va_tracker::mutate` reaches it through a plain function pointer
// and a grep on the symbol enumerates every site that mutates a given
// field.
//
// Cross-TU contract with `va_tracker::mutate`:
//   * Runs inside the engine's nt_pal phase with the leaf held LOCKED.
//   * `new_desc` is a fresh clone already split out for the inside
//     slice; the mutator writes it, and the Swap-CAS that publishes
//     the clone carries the release fence (readers never see a
//     partially-mutated desc).
//   * Every mutator must preserve flag bits it does not name — clones
//     inherit the source's bits, and a later mprotect after e.g.
//     `MADV_DONTFORK` must not lose the DONTFORK bit. The OR / AND-NOT
//     helpers in the .cpp encode this.
//
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

// Context for `brk_extend_mutator`. The placeholder window itself lives
// on the desc (`placeholder_base` / `placeholder_size`) — `new_cursor`
// only carries the moving end of the data segment.
struct BrkExtendCtx {
  // Page-aligned end-of-data-segment cursor. Must lie within the
  // existing `[placeholder_base, placeholder_base + placeholder_size)`
  // window the originating `brk_meta` reserved; outside it, the brk
  // entry rejects before the envelope ever runs.
  void *new_cursor;
};

// Context for `numa_rebind_mutator`. Whole policy in two fields: the
// fault handler's rotating commit path reads `flags & NUMA_INTERLEAVE`
// to decide whether to honour `numa_interleave_mask` at all.
struct NumaRebindCtx {
  // True for `MPOL_INTERLEAVE`; false for every other mode (the caller
  // is expected to pass `nodemask = 0` for `MPOL_DEFAULT` so the mask
  // and the bit drop together).
  bool interleave;
  // Bit N set iff node N is in the policy's set.
  uint32_t nodemask;
};

//===----------------------------------------------------------------------===//
// Mutator declarations.
//
// Every entry matches `va_tracker::DescMutator`:
//   void (*)(RegionDesc *new_desc, void *ctx)
//===----------------------------------------------------------------------===//

// `mlock` / `munlock` immediate-lock case — no-op mutator. Immediate
// locking is tracked by the kernel's per-page lock counter, so no
// per-desc bit exists. `mlock` itself bypasses this entry and calls
// `nt_pal::lock_range` directly (it must walk the kernel VAD, including
// foreign / image VA the va_tracker does not see); the entry is kept so
// the `mutate` envelope stays available for paired `prot_change` use.
void lock_mutator(::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc,
                  void *ctx);

// `mlock2(MLOCK_ONFAULT)` and `mlockall(MCL_ONFAULT)` — set
// `region_flag::LOCK_ONFAULT`. The bit arms lock-on-first-touch; the
// caller separately installs PAGE_GUARD via `nt_pal::arm_guard_trap`
// (the protection write needs the kernel-VAD walk this mutator does
// not own). On first access the kernel raises
// `STATUS_GUARD_PAGE_VIOLATION`, the fault handler resolves the desc,
// sees the bit, and locks.
void lock_set_onfault_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `munlock` — clear `region_flag::LOCK_ONFAULT`. PAGE_GUARD residue on
// already-armed pages is harmless: the kernel auto-clears PAGE_GUARD
// on the trip through the fault handler, and with the bit now clear
// the handler returns `EXCEPTION_CONTINUE_SEARCH` rather than re-locking.
void lock_clear_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `brk` / `sbrk` cursor extension. `ctx` is a `BrkExtendCtx *`. The
// mutator is the metadata step only; the caller passes `prot_change`
// (when non-zero) on the same `mutate` envelope so the engine fires
// `commit_replace` across the newly-uncovered tail under the same hold.
void brk_extend_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `mbind(MPOL_INTERLEAVE)` / `mbind(MPOL_DEFAULT)`. `ctx` is a
// `NumaRebindCtx *`. Pure-metadata mutator (`prot_change` is 0) — the
// fault handler picks up the new mask on the next demand-commit; pages
// already committed under the old policy are not migrated, matching
// Linux `mbind` without `MPOL_MF_MOVE`.
void numa_rebind_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_DONTDUMP` — set `region_flag::DUMP_EXCLUDE`. The desc bit is
// the libc-visible record; the durable kernel-side effect (PEB WER
// gather-list update) is the caller's responsibility.
void dump_set_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_DODUMP` — clear `region_flag::DUMP_EXCLUDE`.
void dump_clear_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_GUARD_INSTALL` — set `region_flag::PROT_GUARD`. The caller in
// `madvise_guard.cpp` issues `NtProtect(PAGE_NOACCESS)` after publish;
// PAGE_NOACCESS rather than PAGE_GUARD because PAGE_GUARD self-clears
// on the first trip through the fault handler.
void guard_set_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_GUARD_REMOVE` — clear `region_flag::PROT_GUARD`. The caller
// restores baseline page protection after the mutator publishes.
void guard_clear_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_DONTFORK` — set `region_flag::DONTFORK`. Consumed by
// `va_tracker_fork_reinit` to skip the region in the child.
void fork_set_dontfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_DOFORK` — clear `region_flag::DONTFORK`.
void fork_clear_dontfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_WIPEONFORK` — set `region_flag::WIPEONFORK`. Consumed by
// `va_tracker_fork_reinit` to zero the region in the child after
// replay.
void fork_set_wipeonfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

// `MADV_KEEPONFORK` — clear `region_flag::WIPEONFORK`.
void fork_clear_wipeonfork_mutator(
    ::LIBC_NAMESPACE::windows::va_tracker::RegionDesc *new_desc, void *ctx);

} // namespace memory_posix
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_MUTATORS_H
