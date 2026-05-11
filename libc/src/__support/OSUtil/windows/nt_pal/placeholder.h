//===-- nt_pal::placeholder — placeholder VA primitives ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Layer 0 PAL — placeholder lifecycle. The kernel's VAD lock makes each
// individual operation atomic; no userspace synchronization is added
// here.
//
// Public surface (per `NTPOSIX_MEMORY_ARCHITECTURE_DESIGN` §6.0):
//
//   * `reserve_placeholder` / `_ex`     — allocate a placeholder.
//   * `reserve_placeholder_at`           — at a specific base.
//   * `reserve_placeholder_32bit`        — low 2 GiB (MAP_32BIT).
//   * `reserve_placeholder_numa`         — NUMA-affined.
//   * `free_placeholder`                 — release entirely.
//   * `split_placeholder`                — split at offset into two.
//   * `coalesce_placeholders`            — merge adjacent placeholders.
//   * `commit_replace`                   — placeholder → committed.
//   * `commit_replace_writewatch`        — placeholder → committed with
//                                          `MEM_WRITE_WATCH` armed.
//                                          Opt-in: callers that need
//                                          incremental dirty enumeration.
//   * `commit_replace_reserve_only`      — placeholder → reserved (no
//                                          commit; for MAP_NORESERVE
//                                          demand-commit via VEH).
//   * `decommit_preserve`                — committed → placeholder.
//   * `preserve_to_placeholder`          — alias for `decommit_preserve`,
//                                          retained for callers that
//                                          need NTSTATUS detail.
//   * `vm_dontneed`                      — MADV_DONTNEED on private
//                                          memory: decommit + recommit.
//   * `free_va`                          — release to MEM_FREE (full or
//                                          sub-range).
//   * `interior_release`                 — release an interior sub-range
//                                          of a private allocation.
//   * `vm_reset` / `vm_reset_undo`       — MADV_FREE / undo.
//
// **`MEM_WRITE_WATCH` is opt-in, not universal.** `commit_replace` does
// NOT arm WW. The kernel rejects every sub-range form of
// `NtFreeVirtualMemory` on a WW-armed VAD with
// `STATUS_FREE_VM_NOT_AT_BASE` — empirically validated on Windows 11
// build 26200 across plain `MEM_RELEASE`, `MEM_RELEASE |
// MEM_PRESERVE_PLACEHOLDER`, and every prior manipulation
// (`NtResetWriteWatch`, `MEM_DECOMMIT`, `NtProtect`, `MEM_RESET`,
// lock/unlock, WS eviction). Universal WW arming would force a costly
// save-restore brute-force on every partial release. Callers that need
// the dirty bitmap (mremap CoW preserve, fork-CoW snapshot arenas,
// dirty-incremental checkpoint workloads) arm it explicitly via
// `commit_replace_writewatch`. Everything else stays kernel-native
// sub-range releasable.
//
// CI grep gate (`libc/utils/depcheck/check_no_mem_commit.py`) enforces
// no other `MEM_COMMIT` call sites in the libc tree.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PLACEHOLDER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PLACEHOLDER_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Bounds-check helper for ULONG conversions.
LIBC_INLINE ULONG to_nt_ulong(SIZE_T value) {
  LIBC_ASSERT(value <= static_cast<SIZE_T>(~static_cast<ULONG>(0)) &&
              "NT size argument exceeds ULONG range");
  return static_cast<ULONG>(value);
}

//===----------------------------------------------------------------------===//
// Reserve
//===----------------------------------------------------------------------===//

// Reserve a placeholder at addr (or system-chosen if nullptr).
[[nodiscard]] LIBC_INLINE void *reserve_placeholder(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  return NT_SUCCESS(st) ? base : nullptr;
}

// Reserve a plain (non-placeholder) MEM_RESERVE region with the
// MEM_WRITE_WATCH bitmap armed at reservation time. Subsequent per-page
// MEM_COMMIT calls inherit dirty tracking — `MEM_WRITE_WATCH` cannot
// combine with `MEM_RESERVE_PLACEHOLDER` in a single call (per
// `MMAP_OPTIMIZATION_RESEARCH.md` §17.8), so this entry point is for
// the cases that need write-watch on the entire reservation but cannot
// pay the placeholder-split cost (one VAD per per-page commit_replace).
//
// Used by Layer 2 `pagemap` for the 128 GiB flat backing array: one
// VAD covers the whole range, per-page MEM_COMMIT keeps the VAD count
// at one, and the dirty bitmap drives reclamation.
[[nodiscard]] LIBC_INLINE void *
reserve_uncommitted_writewatch(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS, nullptr, 0);
  return NT_SUCCESS(st) ? base : nullptr;
}

// Same as `reserve_uncommitted_writewatch` but constrains the reservation
// to end at or below `highest_inclusive`. Used by Layer 2 `buddy_arena`
// to keep its 4 GiB partition VA inside the pagemap's tracked window
// (the pagemap is sized for `pcb.zone0.max_address()`; without this
// constraint NT can hand back a reservation past the pagemap's reach,
// causing every buddy chunk's pagemap entry to land outside the reserved
// pagemap array and AV on the first ACQUIRE-load).
//
// `highest_inclusive` is the highest byte the reservation may include —
// passed straight through to NT's `MEM_ADDRESS_REQUIREMENTS.HighestEndingAddress`
// field. NT requires this to be `(N × allocation_granularity) - 1` for
// some N (i.e., one less than a 64 KiB-aligned boundary); typical
// values like `KUSER_SHARED_DATA.MaximumUserModeAddress` (= our
// `g_pcb.zone0.max_address()`) already satisfy this shape.
[[nodiscard]] LIBC_INLINE void *
reserve_uncommitted_writewatch_below(void *highest_inclusive,
                                      size_t size) {
  if (highest_inclusive == nullptr ||
      reinterpret_cast<uintptr_t>(highest_inclusive) < size)
    return nullptr;
  MEM_ADDRESS_REQUIREMENTS reqs = {};
  reqs.HighestEndingAddress = highest_inclusive;
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterAddressRequirements;
  param.Pointer = &reqs;
  PVOID base = nullptr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS, &param, 1);
  return NT_SUCCESS(st) ? base : nullptr;
}

// Reserve at a system-chosen base.
[[nodiscard]] LIBC_INLINE void *reserve_placeholder(size_t size) {
  return reserve_placeholder(nullptr, size);
}

// Reserve, returning the kernel-rounded actual size.
[[nodiscard]] LIBC_INLINE void *reserve_placeholder_ex(void *addr, size_t size,
                                                       size_t &actual_size) {
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

// Reserve at a specific 64KiB-aligned base address. Returns nullptr if
// the VA is occupied or the address is invalid.
[[nodiscard]] LIBC_INLINE void *reserve_placeholder_at(void *addr,
                                                      size_t size) {
  return reserve_placeholder(addr, size);
}

// Reserve constrained to the low 2 GiB (MAP_32BIT).
[[nodiscard]] LIBC_INLINE void *reserve_placeholder_32bit(size_t size) {
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

// Reserve with NUMA node affinity.
[[nodiscard]] LIBC_INLINE void *reserve_placeholder_numa(void *addr,
                                                         size_t size,
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

// Result of a placeholder reservation. `base` is `nullptr` on failure;
// `status` is the raw NTSTATUS in both the success and failure paths so
// callers that need to differentiate failure modes
// (`STATUS_NO_MEMORY` / `STATUS_COMMITMENT_LIMIT` for global VA
// pressure, `STATUS_INVALID_PARAMETER` for a caller-side bug, the
// unspecified rest) have it without a second syscall. The two are
// always correlated: `NT_SUCCESS(status)` iff `base != nullptr`.
//
// Callers that only need the pointer access `.base`; the implicit
// drop of `.status` is intentional and free (no out-param dance, no
// dummy variable, returned in registers under both ABIs).
struct PlaceholderReservation {
  void *base;
  NTSTATUS status;
};

// Reserve a power-of-2-aligned placeholder. `alignment` must be a
// power of 2 ≥ 64 KiB (NT allocation granularity). Used by the
// partition layer so each 4 GiB partition occupies exactly one
// coarse-pagemap entry.
[[nodiscard]] LIBC_INLINE PlaceholderReservation
reserve_placeholder_aligned(size_t size, size_t alignment) {
  MEM_ADDRESS_REQUIREMENTS reqs = {};
  reqs.Alignment = alignment;
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterAddressRequirements;
  param.Pointer = &reqs;

  PVOID base = nullptr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &param, 1);
  return {NT_SUCCESS(st) ? base : nullptr, st};
}

// Reserve a power-of-2-aligned placeholder with NUMA node affinity.
// Used by the partition layer when a specific NUMA node is requested
// (NUMA-agnostic case takes the non-NUMA path above).
[[nodiscard]] LIBC_INLINE PlaceholderReservation
reserve_placeholder_numa_aligned(size_t size, size_t alignment,
                                 ULONG numa_node) {
  MEM_ADDRESS_REQUIREMENTS reqs = {};
  reqs.Alignment = alignment;
  MEM_EXTENDED_PARAMETER params[2] = {};
  params[0].Type = MemExtendedParameterAddressRequirements;
  params[0].Pointer = &reqs;
  params[1].Type = MemExtendedParameterNumaNode;
  params[1].ULong = numa_node;

  PVOID base = nullptr;
  SIZE_T actual = size;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, params, 2);
  return {NT_SUCCESS(st) ? base : nullptr, st};
}

//===----------------------------------------------------------------------===//
// Split / Coalesce / Free
//===----------------------------------------------------------------------===//

// Split a placeholder at split_offset into two independent placeholders.
LIBC_INLINE bool split_placeholder(void *addr, size_t split_offset) {
  PVOID base = addr;
  SIZE_T size = split_offset;
  return NT_SUCCESS(::NtFreeVirtualMemory(
      NtCurrentProcess(), &base, &size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
}

// Coalesce adjacent placeholders covering [addr, addr+total_size) into a
// single placeholder. Span must be entirely placeholders; the kernel
// verifies and fails atomically otherwise.
LIBC_INLINE NTSTATUS coalesce_placeholders(void *addr, size_t total_size) {
  PVOID base = addr;
  SIZE_T sz = total_size;
  return ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz,
                                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
}

// Release a placeholder entirely; VA returns to MEM_FREE.
LIBC_INLINE bool free_placeholder(void *addr) {
  PVOID base = addr;
  SIZE_T region_size = 0;
  return NT_SUCCESS(::NtFreeVirtualMemory(NtCurrentProcess(), &base,
                                          &region_size, MEM_RELEASE));
}

//===----------------------------------------------------------------------===//
// Atomic state transitions (kernel CAS)
//===----------------------------------------------------------------------===//

// placeholder → committed. No `MEM_WRITE_WATCH` — see the header
// comment for why universal WW arming was abandoned. Callers that need
// the dirty bitmap (mremap CoW preserve, fork CoW snapshot, dirty-
// incremental checkpoint) call `commit_replace_writewatch` below.
// Returns NTSTATUS for conflict detection.
LIBC_INLINE NTSTATUS commit_replace(void *addr, size_t size, DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
      page_prot, nullptr, 0);
}

// placeholder → committed with NUMA-node affinity. NUMA-node hints
// physical-page placement; no WW arming (see `commit_replace`).
//
// Used by `vm_protect.cpp::demand_map_placeholder`, which consults
// the thread's NUMA policy (`set_mempolicy`) at the moment of demand
// commit and steers placement accordingly.
LIBC_INLINE NTSTATUS commit_replace_numa(void *addr, size_t size,
                                          DWORD page_prot, ULONG numa_node) {
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = numa_node;
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
      page_prot, &param, 1);
}

// placeholder → committed, with `MEM_WRITE_WATCH` armed at commit
// time. Opt-in variant of `commit_replace` — pays the cost of being
// VAD-locked against sub-range release (every form of
// `NtFreeVirtualMemory` on a sub-range of a WW-armed VAD returns
// `STATUS_FREE_VM_NOT_AT_BASE`) in exchange for the kernel-maintained
// dirty bitmap (`NtGetWriteWatch`).
//
// Use only for ranges that:
//   * Will never be partially released while the WW arming is live
//     (single-VAD lifecycle that always tears down as a whole), AND
//   * Need incremental dirty enumeration (CoW preserve, snapshot,
//     checkpoint).
//
// If sub-range release is later needed on a WW-armed region, the
// caller must first full-release the VAD and re-commit any survivor
// slices via plain `commit_replace`, copying survivor content
// through scratch. This is significantly more expensive than the
// sub-range release path on a non-WW VAD; that's why WW is opt-in.
LIBC_INLINE NTSTATUS commit_replace_writewatch(void *addr, size_t size,
                                                DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH,
      page_prot, nullptr, 0);
}

// placeholder → committed with NUMA-node affinity and `MEM_WRITE_WATCH`
// armed. Combination of `commit_replace_numa` and
// `commit_replace_writewatch`; same WW caveats apply.
LIBC_INLINE NTSTATUS commit_replace_writewatch_numa(void *addr, size_t size,
                                                     DWORD page_prot,
                                                     ULONG numa_node) {
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = numa_node;
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH,
      page_prot, &param, 1);
}

// placeholder → reserved (no commit). Used for MAP_NORESERVE
// demand-commit via VEH; the fault handler reads AllocationProtect
// from MBI to recommit on touch.
LIBC_INLINE NTSTATUS commit_replace_reserve_only(void *addr, size_t size,
                                                 DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, page_prot, nullptr, 0);
}

// Commit pages within an existing reservation (private or section
// view). Plain MEM_COMMIT — does NOT add MEM_WRITE_WATCH. Reserved for
// the cases where `commit_replace` cannot be used:
//
//   * Re-commit on previously-placeholder-committed private VA after
//     `decommit_private_range` (mmap MAP_FIXED replace path). The
//     write-watch bitmap is allocated at original placeholder-commit
//     time and survives the decommit/recommit cycle, so re-passing the
//     flag is unnecessary (and the kernel ignores it on re-commits).
//   * NORESERVE section views (`MEM_MAPPED` + `MEM_RESERVE`) where
//     MADV_WILLNEED demand-commits a page range. Section views are
//     `MEM_MAPPED`, which is incompatible with `MEM_WRITE_WATCH`.
//   * Stack-growth and image-loader scratch where the reservation was
//     created upstream (`exec_ops`, VEH thread-stack expansion) and
//     this is the per-grow commit syscall.
//
// The Phase 1 invariant ("every private commit through `commit_replace`,
// always with `MEM_WRITE_WATCH`") binds at allocation time. Re-commits
// within an already-allocated region inherit the original flags.
LIBC_INLINE NTSTATUS commit_in_reservation_no_writewatch(void *addr,
                                                          size_t size,
                                                          DWORD page_prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz,
                                      MEM_COMMIT, page_prot, nullptr, 0);
}

// Commit pages within an existing reservation, with NUMA-node hint.
// Same caveats as `commit_in_reservation_no_writewatch` — does NOT add
// `MEM_WRITE_WATCH`; if the underlying reservation was created with
// the flag, tracking continues, otherwise no tracking is established.
//
// Used by the VEH demand-commit handler on `MAP_INTERLEAVE`'d section
// views, where the per-page node is computed from the page's position
// in the view.
LIBC_INLINE NTSTATUS commit_in_reservation_numa(void *addr, size_t size,
                                                 DWORD page_prot,
                                                 ULONG numa_node) {
  MEM_EXTENDED_PARAMETER param = {};
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = numa_node;
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz,
                                      MEM_COMMIT, page_prot, &param, 1);
}

// Fresh private allocation in a single syscall: combined `MEM_RESERVE`
// + `MEM_COMMIT` + `MEM_WRITE_WATCH`. The Phase-1-compliant entry
// point for one-shot internal allocations that aren't going through
// the placeholder lifecycle (process-bootstrap state, IAT patch
// tables, etc.). The bitmap is established at the initial allocation
// so any later decommit/recommit cycle inherits the tracking.
LIBC_INLINE NTSTATUS allocate_private(void **inout_addr, size_t *inout_size,
                                       DWORD page_prot) {
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), inout_addr, inout_size,
      MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, page_prot, nullptr, 0);
}

// Snmalloc-style "lazy commit / notify_using_readonly" reservation. One
// `NtAllocateVirtualMemoryEx` issues `MEM_RESERVE | MEM_COMMIT` with
// `PAGE_READONLY`; NT immediately backs every PTE with the kernel-owned
// shared zero page (one physical frame, mapped read-only into every
// demand-zero PTE in every process). Reads from any page in the range
// return zero and never fault; writes raise `STATUS_ACCESS_VIOLATION`
// from the page fault handler (no copy-on-write — the protection is RO).
//
// Commit charge IS consumed at the syscall (NT enforces strict commit
// charge with no overcommit), but physical RAM is zero until any page
// is upgraded to `PAGE_READWRITE` via `nt_pal::protect()` and written.
// First write after upgrade triggers a private demand-zero allocation
// of a fresh physical frame; the shared zero page is replaced in the
// PTE; from that point on the page costs one private physical frame.
//
// Used by the Layer 2 pagemap (`alloc/pagemap.cpp`) so every entry is
// universally safe to read from any context (SIGSEGV classifier, debug
// probes, `is_libc_pointer`) without holding a Crystalline pin or a
// substrate-membership gate. The "stay committed for life" property
// (snmalloc reference: `snmalloc/src/snmalloc/ds/pagemap.h:236-271`,
// `notify_using_readonly`) eliminates the pagemap-page-decommit race
// by construction.
//
// **CI gate exemption.** This is the ONE site allowed to issue
// `MEM_COMMIT` without `MEM_WRITE_WATCH`. Combining `MEM_WRITE_WATCH`
// with `PAGE_READONLY` is meaningless (writes can't occur until the
// page is upgraded; once upgraded, the page leaves shared-zero
// territory and joins a private RW reservation that satisfies P1.A
// elsewhere). The `check_no_mem_commit.py` allowlist (when added)
// must list this helper by name.
[[nodiscard]] LIBC_INLINE NTSTATUS
reserve_commit_readonly(void **inout_addr, size_t *inout_size) {
  return ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), inout_addr, inout_size,
      MEM_RESERVE | MEM_COMMIT, PAGE_READONLY, nullptr, 0);
}

// committed → placeholder. Returns NTSTATUS for conflict detection.
// Use during split/rollback paths where the placeholder will be
// reused or coalesced.
LIBC_INLINE NTSTATUS preserve_to_placeholder(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz,
                                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

// committed → placeholder (boolean variant). Equivalent to
// `preserve_to_placeholder` but discards the NTSTATUS detail.
LIBC_INLINE bool decommit_preserve(void *addr, size_t size) {
  return NT_SUCCESS(preserve_to_placeholder(addr, size));
}

//===----------------------------------------------------------------------===//
// MADV_DONTNEED — decommit + recommit, guaranteed zero-fill
//===----------------------------------------------------------------------===//

// Decommit then recommit, preserving page protection. Guarantees
// zero-fill on next access; frees commit charge transiently. The
// recommit is a bare `MEM_COMMIT` on the existing reservation — NT
// rejects `MEM_COMMIT | MEM_WRITE_WATCH` on recommit
// (`STATUS_INVALID_PARAMETER`); if the original reservation was armed
// with WW, the bitmap survives on the reservation and the recommitted
// pages re-enter tracking automatically.
//
// Returns `true` only if both the decommit and recommit syscalls
// succeed; on `false` the VA is left decommitted (next access AVs) and
// the caller is responsible for surfacing the failure (MADV_DONTNEED
// maps it to `ENOMEM`).
[[nodiscard]] LIBC_INLINE bool vm_dontneed(void *addr, size_t size,
                                            DWORD prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS dst =
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_DECOMMIT);
  if (!NT_SUCCESS(dst))
    return false;
  base = addr;
  sz = size;
  NTSTATUS cst = ::NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &sz, MEM_COMMIT, prot, nullptr, 0);
  return NT_SUCCESS(cst);
}

//===----------------------------------------------------------------------===//
// Free / partial release
//===----------------------------------------------------------------------===//

// Release an entire allocation (size=0) or a sub-range. For
// sub-ranges, the caller must have decommitted first.
LIBC_INLINE bool free_va(void *addr, size_t size = 0) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_RELEASE));
}

// Release an interior sub-range of a MEM_PRIVATE allocation to MEM_FREE.
//
// Works on committed or decommitted sub-ranges, at any position (head,
// tail, interior), at page granularity. Surviving fragments become
// independent allocations with their own AllocationBase. Used by
// partial-munmap on placeholder-committed MEM_PRIVATE VA.
LIBC_INLINE bool interior_release(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_RELEASE));
}

// Decommit a sub-range of a placeholder-committed MEM_PRIVATE
// allocation. Sub-range becomes MEM_RESERVE (faults on access — correct
// munmap semantics) while the enclosing allocation stays intact.
//
// The partial-munmap primitive for ANON_PLACEHOLDER regions with
// `region_flag::COMMITTED`. For PROT_NONE / NORESERVE placeholders
// use `split_placeholder` + `free_placeholder` instead.
//
// On `MEM_WRITE_WATCH` allocations, `MEM_DECOMMIT` clears dirty
// tracking for the range automatically (NT semantics).
LIBC_INLINE bool decommit_private_range(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_DECOMMIT));
}

// Decommit pages within a plain (non-placeholder) MEM_RESERVE region.
// Pages return to MEM_RESERVE state (commit charge freed), VA stays
// reserved. Re-commit via `commit_in_reservation_no_writewatch` returns
// zero-filled pages.
//
// Distinct from `decommit_private_range` only in the documented
// contract: that variant operates on placeholder-committed VA from
// `commit_replace`; this variant operates on plain `MEM_RESERVE`
// reservations (e.g. those from `reserve_uncommitted_writewatch`).
// Same `NtFreeVirtualMemory(MEM_DECOMMIT)` syscall under the hood.
LIBC_INLINE bool decommit_uncommitted(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(
      ::NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_DECOMMIT));
}

//===----------------------------------------------------------------------===//
// Reset (MADV_FREE)
//===----------------------------------------------------------------------===//

// Mark pages as discardable. Content may survive or be zero-filled on
// next access — no guarantee either way.
LIBC_INLINE void vm_reset(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  ::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz, MEM_RESET,
                               PAGE_READWRITE, nullptr, 0);
}

// Attempt to undo a prior `vm_reset` (MADV_FREE undo). Returns true if
// all pages still contain their original content; false if any page
// was reclaimed (now zero-filled). Safe on non-reset pages — the
// kernel checks per-PTE state and treats non-reset pages as a no-op.
LIBC_INLINE bool vm_reset_undo(void *addr, size_t size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(::NtAllocateVirtualMemoryEx(NtCurrentProcess(), &base, &sz,
                                                 MEM_RESET_UNDO,
                                                 PAGE_READWRITE, nullptr, 0));
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_PLACEHOLDER_H
