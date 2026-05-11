//===-- Process-wide default thread attributes ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/default_attr.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h" // For Thread::DEFAULT_*

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// `cpp::Atomic<size_t>` for stacksize/guardsize. Both operations the
// public API exposes are lock-free:
//   * `get_*` is a single ACQUIRE load.
//   * `bump_*` is a CAS-loop max-update — converge in one round on the
//     overwhelmingly common no-contention path; spin only if a parallel
//     bump from another thread races us, which musl-style usage makes
//     vanishingly rare.
//
// Atomics for primitive types are constexpr-constructible AND
// trivially destructible, so namespace-scope storage is link-time
// zero-init with no dtor — `-Werror=global-constructors` happy by
// construction. Fork inheritance is automatic on every supported
// platform: COW of the .data segment hands the child the parent's
// last-stored value (RtlCloneUserProcess on NTPOSIX, clone() on Linux).
cpp::Atomic<size_t> g_default_stacksize{Thread::DEFAULT_STACKSIZE};
cpp::Atomic<size_t> g_default_guardsize{Thread::DEFAULT_GUARDSIZE};

// CAS-loop max-update. Returns the value left in `slot` after the
// update settles (`size` when our update won, otherwise the larger
// value that pre-empted us).
size_t bump_max(cpp::Atomic<size_t> &slot, size_t size) {
  size_t cur = slot.load(cpp::MemoryOrder::ACQUIRE);
  while (size > cur) {
    if (slot.compare_exchange_weak(cur, size, cpp::MemoryOrder::ACQ_REL,
                                    cpp::MemoryOrder::ACQUIRE))
      return size;
    // CAS failure refreshed `cur`; the while-condition re-tests whether
    // `size` is still larger than the new observed value.
  }
  return cur;
}

} // namespace

size_t get_default_stacksize() {
  return g_default_stacksize.load(cpp::MemoryOrder::ACQUIRE);
}

size_t get_default_guardsize() {
  return g_default_guardsize.load(cpp::MemoryOrder::ACQUIRE);
}

size_t bump_default_stacksize(size_t size) {
  return bump_max(g_default_stacksize, size);
}

size_t bump_default_guardsize(size_t size) {
  return bump_max(g_default_guardsize, size);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
