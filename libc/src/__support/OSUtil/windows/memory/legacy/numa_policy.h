//===-- Per-thread NUMA memory policy for Windows ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread-local NUMA policy state for set_mempolicy/get_mempolicy/mbind.
// Stored in a thread_local to avoid FLS overhead for a rarely-changing value.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NUMA_POLICY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NUMA_POLICY_H

#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// Check if the system has multiple NUMA nodes.
///
/// Reads the sealed Zone 0 topology snapshot (populated once at Tier A
/// Phase 0). On topology-probe failure the snapshot reports
/// `single_node = 1`, which is the conservative degradation — we
/// behave as if running on single-node hardware.
LIBC_INLINE bool is_numa_system() {
  return nt_pal::numa_topology().single_node == 0;
}

/// Per-thread NUMA memory policy. Default is MPOL_DEFAULT (no preference).
/// set_mempolicy modifies this; get_mempolicy reads it; mmap consults it
/// when allocating to select a NUMA node via MemExtendedParameterNumaNode.
struct NumaPolicy {
  int mode;                 // MPOL_DEFAULT, MPOL_PREFERRED, MPOL_BIND, MPOL_INTERLEAVE
  uint64_t nodemask;        // Bitmask of preferred NUMA nodes (up to 64 nodes)
  unsigned long maxnode;    // Number of valid bits in nodemask
};

/// Get the current thread's NUMA policy.
LIBC_INLINE NumaPolicy &get_thread_numa_policy() {
  static thread_local NumaPolicy policy = {MPOL_DEFAULT, 0, 0};
  return policy;
}

/// Select a NUMA node based on the current thread's policy.
/// Returns -1 if no specific node is preferred (use system default).
LIBC_INLINE int select_numa_node() {
  NumaPolicy &policy = get_thread_numa_policy();

  switch (policy.mode) {
  case MPOL_DEFAULT:
    return -1;

  case MPOL_PREFERRED:
  case MPOL_BIND: {
    // Windows MemExtendedParameterNumaNode is a preference hint, not a hard
    // constraint. Unlike Linux MPOL_BIND, allocation won't fail if the
    // preferred node is exhausted — the kernel silently falls back.
    if (policy.nodemask == 0)
      return -1;
    return __builtin_ctzll(policy.nodemask);
  }

  case MPOL_INTERLEAVE: {
    // Round-robin across set bits in the nodemask.
    static thread_local unsigned int interleave_counter = 0;
    if (policy.nodemask == 0)
      return -1;

    int count = __builtin_popcountll(policy.nodemask);
    if (count == 0)
      return -1;

    int target = static_cast<int>(interleave_counter++ % count);

    // Find the target-th set bit
    uint64_t mask = policy.nodemask;
    for (int i = 0; i < target; ++i)
      mask &= mask - 1; // Clear lowest set bit
    return __builtin_ctzll(mask);
  }

  default:
    return -1;
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NUMA_POLICY_H
