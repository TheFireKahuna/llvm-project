//===- mprotect.cpp - POSIX mprotect / pkey_mprotect ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/mprotect.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/numa_policy.h"
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
  // size == 0 succeeds even with a bogus `addr` — Linux contract that
  // glibc's mprotect test suite pins.
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

  // `mutate` with a null mutator + `commit_if_uncommitted_accessible`
  // drives per-chunk dispatch under the per-succ LOCKED hold: committed
  // pages get `nt_pal::protect`, uncommitted+accessible chunks get
  // demand-commit (mapped → commit-in-reservation, private →
  // commit_replace[_numa]), MEM_FREE mid-range aborts with ENOMEM. The
  // clone's `view_prot` is NOT updated — the kernel holds authoritative
  // per-page protection and consumers query MBI when they need it, so
  // mprotect leaves no desc-state fragmentation behind. First-failure
  // aborts; already-protected chunks are not rolled back (matches Linux).
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
  // ARM64 needs an explicit I-cache invalidation after pages flip
  // executable. x86_64 self-snoops on instruction fetch (SDM Vol. 3A
  // §11.6) so the I/D cache pair stays coherent without software help.
  if (prot & PROT_EXEC)
    ::NtFlushInstructionCache(NtCurrentProcess(), addr,
                               static_cast<SIZE_T>(rounded));
#endif

  return 0;
}

intptr_t pkey_mprotect(void *addr, size_t size, int prot, int pkey) {
  // The size-zero short-circuit precedes the x86_64 gate so a portable
  // app passing size=0 on non-x86_64 still succeeds.
  if (LIBC_UNLIKELY(addr == nullptr)) {
    if (size > 0)
      return -EINVAL;
    return 0;
  }
  if (size == 0)
    return 0;

  // Base protection change runs first; on failure pkey state is not
  // touched. Matching Linux, a successful protection change followed by
  // pkey-registration failure does NOT roll back the protection.
  if (intptr_t rc = mprotect(addr, size, prot); rc != 0)
    return rc;

  // pkey == -1 is the Linux sentinel for "no key tagging" — equivalent
  // to plain mprotect once the base protection change has landed.
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
