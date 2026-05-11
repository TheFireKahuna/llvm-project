//===- partition_numa_select.h - NUMA node selector ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Hot-path NUMA node selector for partition reservations.
///
/// Answers a single question: "which NUMA node should this allocation's
/// partition reserve land on?" The decision is sticky per thread once
/// resolved — successive allocations on the same thread reuse the cached
/// node id so freshly-allocated chunks tend to fault in on the same node
/// that owns the thread's current cache lines.
///
/// The selector composes three stages:
///
///   1. Class-band short-circuit. Only the user-facing
///      \c AllocSmall..AllocHuge band replicates per node. The pinned
///      bands (core libc-internal and VA-index) were reserved once on the
///      node-agnostic key at process bring-up; any subsequent lookup must
///      use the same key or a duplicate partition is published and the
///      first-writer-wins invariant on the pagemap breaks.
///
///   2. Single-node short-circuit. A boolean cached in the sealed NUMA
///      topology snapshot is true when the system has exactly one node,
///      or when the topology probe failed and the safe degradation took
///      the system as effectively single-node. Single-node systems take
///      a one-byte-load + branch path with zero atomics, no syscalls,
///      and no per-thread state.
///
///   3. Lazy per-thread resolution on multi-node systems. The thread's
///      preferred node is cached in its lifecycle record. First touch
///      reads the current logical processor via
///      \c NtGetCurrentProcessorNumberEx, indexes the sealed cpu-to-node
///      table, and stashes the result; every subsequent allocation on
///      that thread reads the cached byte.
///
/// NT topology is enumerated via \c ThreadSelectedCpuSets (multi-group
/// affinity, the only API that gives a correct answer across processor
/// group boundaries) rather than the legacy \c SetThreadAffinityMask
/// which collapses to a single processor group and misreports topology
/// on systems with > 64 logical CPUs.
///
/// The selector runs only inside the partition layer's slow path
/// (\c reserve_or_grow) — never inside \c partition::lookup's fast path —
/// so a syscall on the first call per thread amortises across every
/// allocation served from the resulting 4 GiB partition.
///
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

/// True iff \p cls is one of the four user-facing allocator classes that
/// replicate per NUMA node.
///
/// Every other class — core libc-internal and VA-index — was reserved at
/// process bring-up against \c kNodeAgnostic and must be looked up with
/// that same key.
[[nodiscard]] LIBC_INLINE constexpr bool is_user_replicable(PartitionClass cls) {
  uint16_t v = static_cast<uint16_t>(cls);
  return v >= static_cast<uint16_t>(PartitionClass::AllocSmall) &&
         v <= static_cast<uint16_t>(PartitionClass::AllocHuge);
}

/// Resolve the calling thread's preferred NUMA node and stash the result
/// in \p lc for subsequent calls.
///
/// Reads the current logical processor with
/// \c NtGetCurrentProcessorNumberEx (the only API that returns a correct
/// answer across processor groups), folds <tt>Group * 64 + Number</tt>
/// into a flat index, and indexes the sealed cpu-to-node table. If the
/// index is out of range or the probe left no mapping for that CPU, the
/// safe fallback is node 0 — every active node is a valid target for any
/// thread, just suboptimal for cache locality.
///
/// \p topo aliases \c g_pcb.zone0.numa_topology(); the storage is
/// hardware-immutable for the process lifetime so the read is plain.
///
/// \returns the NUMA node id that subsequent allocations on this thread
///          should bias toward.
[[nodiscard]] LIBC_INLINE uint8_t
resolve_thread_preferred_node(const NumaTopology &topo, ThreadLifecycle *lc) {
  PROCESSOR_NUMBER pn{};
  (void)::NtGetCurrentProcessorNumberEx(&pn);
  uint32_t idx = static_cast<uint32_t>(pn.Group) * 64u +
                 static_cast<uint32_t>(pn.Number);
  uint8_t node;
  if (LIBC_LIKELY(idx < kNumaCpuTableSize)) {
    node = topo.cpu_to_node[idx];
    if (LIBC_UNLIKELY(node == kNumaNodeUnassigned))
      node = 0;
  } else {
    node = 0;
  }
  // RELAXED: the cache is owner-written. A peer reading either the
  // unresolved sentinel or a previous resolved value both decode to a
  // valid partition selection on the next allocation; no happens-before
  // relationship needs to be published.
  lc->preferred_node.store(node, cpp::MemoryOrder::RELAXED);
  return node;
}

/// Pick the NUMA node id for a fresh partition reservation of class
/// \p cls.
///
/// \returns \c kNodeAgnostic for non-replicating classes and on
///          single-node systems; otherwise the calling thread's cached
///          (and lazily resolved) preferred node.
[[nodiscard]] LIBC_INLINE uint16_t pick_node_for_alloc(PartitionClass cls) {
  if (LIBC_LIKELY(!is_user_replicable(cls)))
    return kNodeAgnostic;

  const NumaTopology &topo = nt_pal::numa_topology();
  if (LIBC_LIKELY(topo.single_node != 0))
    return kNodeAgnostic;

  // The lifecycle pointer comes from the TEB TLS slot. Foreign-thread
  // entry points (signal init, robust-mutex first touch, dlopen module
  // entry) can reach here before any lifecycle is installed. Without a
  // lifecycle there is nowhere to cache the resolution result, so fall
  // back to kNodeAgnostic rather than re-probe on every allocation.
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
