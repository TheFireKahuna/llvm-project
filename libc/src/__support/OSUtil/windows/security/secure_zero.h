//===-- secure_zero — DSE-proof, cache-evicting zero wipe -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_SECURE_ZERO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_SECURE_ZERO_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/size_t.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h" // LIBC_UNLIKELY
#include "src/string/memory_utils/inline_memset.h"

#if defined(__x86_64__) && defined(__CLFLUSHOPT__)
#include <immintrin.h>
#endif

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Zero [p, p+n) so that the secret is unrecoverable even by an attacker
// observing L1/L2 cache state after return.
//
// Three layers:
//   1. inline_memset writes 0 (uses AVX2 fast path on v3 baseline).
//   2. asm memory-clobber barrier prevents DSE eliminating the wipe when
//      the buffer's address does not escape the current TU. This is the
//      same idiom memset_explicit uses at src/string/memset_explicit.cpp.
//   3. CLFLUSHOPT drops every cache line the wipe touched. SFENCE then
//      orders the flushes against subsequent stores. Without the fence
//      a later unrelated store may retire before the eviction drains
//      and a transient-execution attacker on this core can still see
//      the pre-wipe value.
//
// Scope:
//   - This-core defense. A concurrent reader on another core already in
//     S-state has already observed the secret; flushing comes too late.
//     Reach for pkey isolation, not secure_zero, for multi-core threats.
//   - Not cryptographically constant-time. Callers that need that must
//     also avoid data-dependent branching around secret bytes.
//   - CLFLUSHOPT is a no-op on write-combining / uncacheable memory;
//     safe to call on any mapping type.
[[gnu::noinline]] LIBC_INLINE void secure_zero(void *p, size_t n) {
  if (LIBC_UNLIKELY(n == 0))
    return;

  inline_memset(p, 0, n);

  // DSE barrier. Must precede the flush loop: without it, the optimizer
  // may eliminate inline_memset (no observer of the stores) *and keep*
  // the CLFLUSHOPTs, evicting whatever happened to live there instead
  // of the wipe.
  asm volatile("" : : "r"(p) : "memory");

#if defined(__x86_64__) && defined(__CLFLUSHOPT__)
  constexpr uintptr_t kLine = 64;
  uintptr_t base = reinterpret_cast<uintptr_t>(p);
  uintptr_t lo = base & ~(kLine - 1);
  uintptr_t hi = (base + n + kLine - 1) & ~(kLine - 1);
  for (uintptr_t a = lo; a < hi; a += kLine)
    _mm_clflushopt(reinterpret_cast<const void *>(a));
  // CLFLUSHOPT is ordered only against writes to the same line and
  // against other CLFLUSHOPTs to the same line. SFENCE is required
  // to fence it against arbitrary subsequent memory accesses.
  _mm_sfence();
#endif
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_SECURE_ZERO_H
