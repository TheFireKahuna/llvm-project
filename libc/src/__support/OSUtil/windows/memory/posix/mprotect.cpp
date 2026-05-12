//===- mprotect.cpp - POSIX mprotect / pkey_mprotect ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two surfaces: `internal::mprotect(addr, len, prot)` for plain
// protection updates, and `internal::pkey_mprotect(addr, len, prot,
// pkey)` for the pkey-tagged variant. Linux contract:
//   - `mprotect(size=0)` is a no-op success regardless of `addr`.
//   - Cross-VAD failure is first-failure-aborts; no rollback of
//     already-protected chunks. Matches glibc behaviour.
//   - `pkey_mprotect` runs the base mprotect first and registers the
//     range only on success; registration failure does NOT roll back
//     the protection change. Matches Linux.
//
// Dispatch shape: one shape gate at entry. If the request hits a
// single tracked region of qualifying shape with no NORESERVE bit,
// the fast path issues one `va_tracker::mutate` call — the
// substrate's envelope updates the desc(s) AND runs per-VAD
// `nt_pal::protect` post-Swap. Everything else (mid-region
// page-granular requests, NORESERVE backings that may still hold
// uncommitted chunks, cross-VAD spans containing holes, foreign
// VAs the tracker does not know about) takes the slow path: a
// `RegionWalker` over the kernel VAD chain dispatches each chunk by
// its committed / uncommitted / MEM_FREE state. The slow path
// preserves the legacy three-way uncommitted dispatch verbatim:
// uncommitted + PROT_NONE → no-op, uncommitted + accessible +
// MEM_MAPPED → commit in reservation, uncommitted + accessible +
// MEM_PRIVATE → demand-map placeholder with thread NUMA policy.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mprotect.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/legacy/numa_policy.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_errno.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_mutators.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/security/pkey_state.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/macros/properties/architectures.h"

namespace LIBC_NAMESPACE_DECL {

namespace {

namespace mp = ::LIBC_NAMESPACE::windows::memory_posix;
namespace vt = ::LIBC_NAMESPACE::windows::va_tracker;

// Translate the POSIX-derived `PAGE_READWRITE` / `PAGE_EXECUTE_READWRITE`
// pair into their CoW counterparts. MAP_PRIVATE file views are backed
// by a section that may have been created with only read access (read-
// only fd); `PAGE_WRITECOPY` lets writes trigger kernel CoW without
// requiring section write access. Source of truth is the durable
// `region_flag::COW` bit stamped at acquire time.
LIBC_INLINE DWORD cow_translate(DWORD new_prot, bool is_cow) {
  if (!is_cow)
    return new_prot;
  if (new_prot == PAGE_READWRITE)
    return PAGE_WRITECOPY;
  if (new_prot == PAGE_EXECUTE_READWRITE)
    return PAGE_EXECUTE_WRITECOPY;
  return new_prot;
}

// True if `shape + flags` qualifies for the single-mutate fast path.
// Only fully-committed shapes can take a single NtProtect call without
// hitting STATUS_NOT_COMMITTED on a mid-range demand-commit page.
// NORESERVE forces the slow path regardless of shape.
LIBC_INLINE bool fast_path_eligible(vt::RegionShape shape, uint16_t flags) {
  using S = vt::RegionShape;
  if (flags & vt::region_flag::NORESERVE)
    return false;
  switch (shape) {
  case S::ANON_PLACEHOLDER:
    // Bare PROT_NONE placeholder (no COMMITTED bit) takes the slow
    // path; the slow path's three-way dispatch decides between no-op
    // and demand-map.
    return (flags & vt::region_flag::COMMITTED) != 0;
  case S::FILE_VIEW_MONO:
  case S::FILE_VIEW_CHUNKED:
    return true;
  default:
    return false;
  }
}

// Context shared between the walk_range visitor and the fast-path
// gate. `region_count` counts visits; `single_covered` retains the
// covered interval of the single visited region (only valid when
// region_count == 1); `desc_view_prot`, `desc_flags`, `desc_shape`
// snapshot the leading desc's fields.
struct FastPathProbe {
  uint32_t region_count;
  vt::VaRange single_covered;
  DWORD desc_view_prot;
  uint16_t desc_flags;
  uint16_t desc_shape;
};

// walk_range visitor. Stops collecting after the second region — the
// fast path requires exactly one. The walk itself cannot abort early
// (the substrate's walk does not consult the visitor's return value),
// so the visitor short-circuits on its own bookkeeping.
void fast_path_visitor(vt::VaRange covered, vt::RegionDesc *desc,
                       void *ctx) {
  auto *probe = static_cast<FastPathProbe *>(ctx);
  if (probe->region_count == 0) {
    probe->single_covered = covered;
    probe->desc_view_prot = desc->view_prot;
    probe->desc_flags = desc->flags_load();
    probe->desc_shape = static_cast<uint16_t>(desc->current_shape());
  }
  if (probe->region_count != UINT32_MAX)
    ++probe->region_count;
}

// Slow-path per-chunk dispatch. Three-way uncommitted handling plus
// committed-prot-change with CFG retry. Mirrors the legacy
// `protect_chunk` row-for-row; differences:
//   - The desc lookup is via `va_tracker::resolve` (not the legacy
//     mapping table snapshot).
//   - `VM_FLAG_PROT_CHANGED` stamping is dropped — that legacy
//     slot-flag mechanism is absorbed by the substrate's per-skiplist-
//     node LOCKED byte. If the fault handler later needs a
//     "protection diverged" hint, the substrate exposes it as a
//     region_flag bit; the POSIX layer stamps via mutate.
int protect_chunk_slow(void *addr, SIZE_T size, DWORD new_prot, int posix_prot,
                       const MEMORY_BASIC_INFORMATION &mbi) {
  const bool committed = (mbi.State == MEM_COMMIT);

  if (!committed && posix_prot != PROT_NONE) {
    // Uncommitted + accessible target: section view stays inside its
    // existing reservation; bare placeholder takes a demand-map.
    if (mbi.Type == MEM_MAPPED) {
      NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_in_reservation_no_writewatch(
          addr, size, new_prot);
      if (NT_ERROR(st))
        return -::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(st);
      return 0;
    }

    // MEM_PRIVATE + uncommitted + accessible — bare PROT_NONE
    // placeholder upgrade. The thread's NUMA policy decides
    // physical-page placement; on NUMA-hint failure fall back to
    // unhinted commit so the mapping is not lost.
    int node = ::LIBC_NAMESPACE::windows::select_numa_node();
    if (node >= 0) {
      NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_replace_numa(
          addr, size, new_prot, static_cast<ULONG>(node));
      if (NT_SUCCESS(st))
        return 0;
    }
    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_replace(addr, size,
                                                            new_prot);
    if (NT_ERROR(st))
      return -::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(st);
    return 0;
  }

  // Uncommitted + PROT_NONE: already inaccessible. Skip the syscall —
  // NtProtect on uncommitted pages returns STATUS_NOT_COMMITTED.
  if (!committed)
    return 0;

  // Committed: translate to WRITECOPY when the underlying desc is COW
  // (or, for untracked / image VADs, when AllocationProtect already
  // names a write-copy variant). The desc is the source of truth for
  // tracked mappings; AllocationProtect is the fallback for foreign
  // ones.
  bool needs_cow = false;
  if (auto ref = vt::resolve(addr); ref.has_value()) {
    needs_cow = ref.value().desc->has_flag(vt::region_flag::COW);
  } else if (mbi.Type == MEM_MAPPED) {
    DWORD alloc_prot = mbi.AllocationProtect & 0xFFu;
    needs_cow = (alloc_prot == PAGE_WRITECOPY ||
                 alloc_prot == PAGE_EXECUTE_WRITECOPY);
  }
  DWORD effective = cow_translate(new_prot, needs_cow);

  PVOID base = addr;
  SIZE_T region_size = size;
  ULONG old_protect = 0;
  NTSTATUS st = ::NtProtectVirtualMemory(NtCurrentProcess(), &base,
                                          &region_size, effective,
                                          &old_protect);

  // CFG-secured / driver-locked ranges cache the prior protection;
  // flushing the cache lets the next NtProtect see live state. The
  // retry uses the original `new_prot` (not the COW-translated
  // value) — matches the legacy retry shape on lines 685-693 where
  // the flush retry was sometimes the path that unblocked a CFG
  // range whose first attempt failed the COW translation entirely.
  if (st == STATUS_INVALID_PAGE_PROTECTION) {
    if (::RtlFlushSecureMemoryCache(addr, size)) {
      base = addr;
      region_size = size;
      st = ::NtProtectVirtualMemory(NtCurrentProcess(), &base, &region_size,
                                     new_prot, &old_protect);
    }
  }
  if (NT_ERROR(st))
    return -::LIBC_NAMESPACE::windows_util::ntstatus_to_errno(st);

#ifdef LIBC_TARGET_ARCH_IS_AARCH64
  // I-cache invalidation after pages become executable. No-op on
  // x86_64 (the kernel's IPI handles cross-CPU visibility there).
  if (posix_prot & PROT_EXEC)
    ::NtFlushInstructionCache(NtCurrentProcess(), addr, size);
#else
  (void)posix_prot;
#endif

  return 0;
}

// Top-level dispatcher. Returns 0 on success or a Linux `-errno` value
// (negative). All validation has been done by the caller; this
// function owns the fast-path / slow-path split only.
intptr_t mprotect_dispatch(void *addr, size_t rounded_size, int posix_prot,
                           DWORD new_prot) {
  const uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  const size_t alloc_gran =
      ::LIBC_NAMESPACE::windows::get_alloc_granularity();
  const bool gran_aligned =
      ((addr_val & (alloc_gran - 1)) == 0) &&
      ((rounded_size & (alloc_gran - 1)) == 0);

  if (gran_aligned) {
    vt::VaRange range = mp::make_range(addr, rounded_size);
    FastPathProbe probe{0, vt::VaRange{}, 0, 0, 0};
    vt::walk_range(range, &fast_path_visitor, &probe);

    const bool single_region = (probe.region_count == 1);
    const bool covers_full =
        single_region &&
        probe.single_covered.lo() == range.lo() &&
        probe.single_covered.hi() == range.hi();
    const bool shape_ok =
        single_region &&
        fast_path_eligible(static_cast<vt::RegionShape>(probe.desc_shape),
                            probe.desc_flags);

    if (single_region && covers_full && shape_ok) {
      DWORD effective = cow_translate(
          new_prot, (probe.desc_flags & vt::region_flag::COW) != 0);
      int rc = vt::mutate(range, &mp::prot_mutator, &effective,
                          /*prot_change=*/effective);
      if (rc == 0) {
#ifdef LIBC_TARGET_ARCH_IS_AARCH64
        if (posix_prot & PROT_EXEC)
          ::NtFlushInstructionCache(NtCurrentProcess(), addr,
                                     static_cast<SIZE_T>(rounded_size));
#endif
        return 0;
      }
      // Fall through to the slow path on substrate-side rejection
      // (alignment surprise, race, ENOTSUP for heterogeneous siblings)
      // so a single VAD-level retry never surfaces -errno to the user
      // unnecessarily.
    }
  }

  // Slow path: per-chunk dispatch over the kernel VAD chain. MEM_FREE
  // inside the range is hard-fail with ENOMEM (distinct from munmap's
  // hole tolerance). First-failure-aborts; no rollback of chunks
  // already protected — matches Linux.
  auto ws = ::LIBC_NAMESPACE::windows::byte_scratch(4096);
  if (!ws)
    return -ENOMEM;
  ::LIBC_NAMESPACE::nt_pal::RegionWalker walk(addr,
                                               static_cast<SIZE_T>(rounded_size),
                                               ws.data(), ws.size());
  while (walk.next()) {
    if (LIBC_UNLIKELY(walk.entry->State == MEM_FREE))
      return -ENOMEM;
    int rc = protect_chunk_slow(walk.chunk, walk.chunk_size, new_prot,
                                 posix_prot, *walk.entry);
    if (rc != 0)
      return rc;
  }
  return 0;
}

} // namespace

namespace internal {

intptr_t mprotect(void *addr, size_t size, int prot) {
  // size == 0 is a no-op success regardless of `addr` — Linux contract
  // some glibc tests depend on. Validate first; null + nonzero is
  // EINVAL.
  if (size == 0)
    return 0;
  if (LIBC_UNLIKELY(addr == nullptr))
    return -EINVAL;
  if (LIBC_UNLIKELY(!mp::is_page_aligned(addr)))
    return -EINVAL;
  if (int e = mp::validate_mprotect_prot(prot); e != 0)
    return -e;

  const size_t rounded = mp::rounded_len_or_zero(size);
  if (LIBC_UNLIKELY(rounded == 0))
    return -ENOMEM;
  const uintptr_t addr_val = reinterpret_cast<uintptr_t>(addr);
  if (LIBC_UNLIKELY(mp::addr_plus_len_overflows(addr_val, rounded)))
    return -ENOMEM;

  const DWORD new_prot = mp::posix_prot_to_page(prot);
  return mprotect_dispatch(addr, rounded, prot, new_prot);
}

intptr_t pkey_mprotect(void *addr, size_t size, int prot, int pkey) {
  // addr == NULL + size == 0 → 0; addr == NULL + size > 0 → EINVAL.
  // The size-zero branch returns before the architecture gate so a
  // portable app using pkey_mprotect on non-x86_64 with size=0 still
  // succeeds.
  if (LIBC_UNLIKELY(addr == nullptr)) {
    if (size > 0)
      return -EINVAL;
    return 0;
  }
  if (size == 0)
    return 0;

  // Run the base mprotect first; on failure pkey is not touched.
  if (intptr_t rc = mprotect(addr, size, prot); rc != 0)
    return rc;

  if (pkey == -1)
    return 0;

#ifndef LIBC_TARGET_ARCH_IS_X86_64
  (void)pkey;
  return -ENOSYS;
#else
  if (LIBC_UNLIKELY(pkey < 0 || pkey >= ::LIBC_NAMESPACE::windows::PKEY_COUNT))
    return -EINVAL;
  uint32_t alloc_bits =
      ::LIBC_NAMESPACE::g_pcb.pkey.allocated.load(cpp::MemoryOrder::RELAXED);
  if (LIBC_UNLIKELY(!(alloc_bits & (1u << pkey))))
    return -EINVAL;

  if (!::LIBC_NAMESPACE::windows::pkey_register_range(
          addr, static_cast<SIZE_T>(size), pkey, prot))
    return -ENOMEM;

  return 0;
#endif
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
