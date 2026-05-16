//===- partition_numa_select.h - NUMA node selector ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hot-path NUMA node selector for partition reservations.
//
// One question: which NUMA node should this allocation's partition reserve
// land on? The decision is sticky per thread once resolved -- successive
// allocations reuse the cached node id so freshly-allocated chunks tend to
// fault in on the same node that owns the thread's hot cache lines.
//
// Three stages:
//   1. Class-band short-circuit. Only AllocSmall..AllocHuge replicates per
//      node. Pinned bands (core libc-internal, VA-index) publish on the
//      node-agnostic key (eager at bring-up, or demand on first use, but
//      always kNodeAgnostic); any later lookup must reuse that key or a
//      duplicate partition is published and the pagemap's
//      first-writer-wins invariant breaks.
//   2. Single-node short-circuit. A bool cached in the sealed NUMA
//      topology snapshot is true on actual single-node systems and on
//      probe failure (safe degrade). One-byte load + branch, zero atomics,
//      no syscalls, no per-thread state.
//   3. Lazy per-thread resolution on multi-node. The preferred node is
//      cached in the thread lifecycle record. First touch reads the
//      current logical processor via NtGetCurrentProcessorNumberEx, indexes
//      the sealed cpu-to-node table, and stashes; subsequent allocations
//      on the same thread read the cached byte.
//
// Topology comes from ThreadSelectedCpuSets (multi-group affinity, the only
// API that returns a correct answer across processor groups); legacy
// SetThreadAffinityMask collapses to one group and misreports on >64-CPU
// systems.
//
// The selector runs only inside the partition layer's slow path
// (reserve_or_grow) -- never inside partition::lookup's fast path -- so a
// syscall on the first call per thread amortises across every allocation
// served from the resulting 4 GiB partition.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PARTITION_NUMA_SELECT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PARTITION_NUMA_SELECT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/nt_pal/numa_topology.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/thread_lifecycle.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace partition {

// True iff cls is one of the four user-facing allocator classes that
// replicate per NUMA node. Pinned bands publish against kNodeAgnostic and
// must be looked up with that same key.
[[nodiscard]] LIBC_INLINE constexpr bool is_user_replicable(PartitionClass cls) {
  uint16_t v = static_cast<uint16_t>(cls);
  return v >= static_cast<uint16_t>(PartitionClass::AllocSmall) &&
         v <= static_cast<uint16_t>(PartitionClass::AllocHuge);
}

// Resolve and cache the calling thread's preferred NUMA node. topo aliases
// g_pcb.zone0.numa_topology() -- hardware-immutable for process lifetime,
// so the reads are plain.
[[nodiscard]] LIBC_INLINE uint8_t
resolve_thread_preferred_node(const NumaTopology &topo, ThreadLifecycle *lc) {
  PROCESSOR_NUMBER pn{};
  (void)::NtGetCurrentProcessorNumberEx(&pn);
  uint32_t idx = static_cast<uint32_t>(pn.Group) * 64u +
                 static_cast<uint32_t>(pn.Number);
  uint8_t node;
  if (LIBC_LIKELY(idx < kNumaCpuTableSize)) {
    node = topo.cpu_to_node[idx];
    // Fallback to node 0 on out-of-range CPU id or unmapped slot: every
    // active node is a valid target, just suboptimal for cache locality.
    if (LIBC_UNLIKELY(node == kNumaNodeUnassigned))
      node = 0;
  } else {
    node = 0;
  }
  // RELAXED: cache is owner-written. A peer reading either the unresolved
  // sentinel or a stale resolved value still decodes to a valid partition
  // selection -- no happens-before needs publishing.
  lc->preferred_node.store(node, cpp::MemoryOrder::RELAXED);
  return node;
}

// Returns kNodeAgnostic for non-replicating classes and on single-node
// systems; otherwise the calling thread's cached (lazily resolved) node.
[[nodiscard]] LIBC_INLINE uint16_t pick_node_for_alloc(PartitionClass cls) {
  if (LIBC_LIKELY(!is_user_replicable(cls)))
    return kNodeAgnostic;

  const NumaTopology &topo = nt_pal::numa_topology();
  if (LIBC_LIKELY(topo.single_node != 0))
    return kNodeAgnostic;

  // Foreign-thread entry points (signal init, robust-mutex first touch,
  // dlopen module entry) can reach here before any lifecycle is installed
  // in the TEB TLS slot. Without one there is nowhere to cache the result;
  // returning kNodeAgnostic avoids re-probing on every allocation.
  ThreadLifecycle *lc = LIBC_NAMESPACE::get_current_lifecycle();
  if (LIBC_UNLIKELY(lc == nullptr))
    return kNodeAgnostic;

  uint8_t cached = lc->preferred_node.load(cpp::MemoryOrder::RELAXED);
  if (LIBC_LIKELY(cached != LIBC_NAMESPACE::kPreferredNodeUnresolved))
    return cached;
  return resolve_thread_preferred_node(topo, lc);
}

} // namespace partition
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
