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
// Dispatch is pre-split + one substrate call. The POSIX layer pre-
// splits the descs at the protect range's edges so the mutate
// envelope sees only full-cover succs — every clone gets an accurate
// `view_prot` update and `region_flag::PROT_DIVERGED` is never
// stamped. The accurate-view_prot invariant matters for fork replay
// (`replay_emit` in `va_tracker.cpp` passes `desc->view_prot` to the
// child's `acquire` meta) and for any future mremap that preserves
// protection across the move; PROT_DIVERGED would leak the original
// protection into the new mapping.
//
// The mutate envelope still handles per-chunk dispatch under the
// full-cover succs (committed → protect, uncommitted + accessible
// + MEM_MAPPED → commit_in_reservation, uncommitted + accessible +
// MEM_PRIVATE → commit_replace[_numa]), MEM_FREE rejection, and
// per-succ COW translation — all under the per-succ LOCKED hold so
// a concurrent va_tracker mutator cannot race the demand-map step.
//
// The POSIX layer keeps three responsibilities the substrate has no
// reason to learn: entry validation, NUMA-node selection from the
// thread's `set_mempolicy` state, and the ARM64 I-cache flush after
// a successful PROT_EXEC change. CFG-secured retries are absorbed
// by `nt_pal::protect` itself, so this file has no
// `RtlFlushSecureMemoryCache` reference either.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mprotect.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/legacy/numa_policy.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_meta.h"
#include "src/__support/OSUtil/windows/memory/posix/posix_validation.h"
#include "src/__support/OSUtil/windows/memory/va_tracker.h"
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
  const int numa_node = ::LIBC_NAMESPACE::windows::select_numa_node();

  // Pre-split at the protect range's edges so the mutate envelope
  // sees only fully-covered succs. A boundary that lands on an
  // existing desc edge or inside a MEM_FREE hole returns ENOENT /
  // EINVAL from `split` — both mean "no straddle here, nothing to
  // do" and are silently tolerated. Only a strict-interior boundary
  // triggers an actual split. Same idiom as `munmap.cpp`.
  const uintptr_t addr_lo = addr_val;
  const uintptr_t addr_hi = addr_val + rounded;
  (void)vt::split(reinterpret_cast<void *>(addr_lo));
  (void)vt::split(reinterpret_cast<void *>(addr_hi));

  const vt::VaRange range = mp::make_range(addr, rounded);
  const int rc = vt::mutate(range,
                            /*mutator=*/nullptr,
                            /*ctx=*/nullptr,
                            /*prot_change=*/new_prot,
                            /*commit_if_uncommitted_accessible=*/true,
                            /*numa_node=*/numa_node);
  if (rc != 0)
    return -rc;

#ifdef LIBC_TARGET_ARCH_IS_AARCH64
  // I-cache invalidation after pages become executable. No-op on
  // x86_64 (the kernel's IPI handles cross-CPU visibility there).
  if (prot & PROT_EXEC)
    ::NtFlushInstructionCache(NtCurrentProcess(), addr,
                               static_cast<SIZE_T>(rounded));
#endif

  return 0;
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
