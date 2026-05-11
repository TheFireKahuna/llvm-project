//===-- nt_pal::NumaTopology — sealed cpu→node snapshot ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-lifetime NUMA topology snapshot. Built once at libc init via
// `NtQuerySystemInformationEx(SystemLogicalProcessorInformationEx,
// RelationNumaNode, ...)` and stored inline inside `PcbZone0`. After the
// Tier A seal the bytes are hardware-immutable (PAGE_READONLY) for the
// process lifetime, so:
//
//   * Hot-path readers (the partition-layer NUMA selector, sched_getcpu,
//     numa_ops) avoid the ~50 ns syscall + 4 KiB scratch alloc that the
//     previous on-demand `find_numa_node()` paid on every call.
//
//   * An arbitrary-write attacker cannot rewrite `cpu_to_node` or
//     `single_node` to steer NUMA-affined reservations into adversary-
//     controlled VA replicas — same threat model as the partition coarse
//     pagemap pointer and the buddy arena secret already share.
//
//   * Fork inheritance is automatic (Zone 0 CoW-shares unchanged); NT
//     preserves NUMA layout across `RtlCloneUserProcess`, so no rebuild
//     in `pal_fork_reinit`.
//
// Layout choice: 256 entries × 1 B + 8 B header = 264 B fits well under
// the Zone 0 page budget. The `cpu_to_node[]` table is direct-addressed
// by `Group * 64 + Number` (matching `PROCESSOR_NUMBER` packing), so
// hot-path lookup is one byte-load with no branch on the common path.
//
// Node-id width: NT exposes `MAXIMUM_NODE_COUNT = 0x40` on x64
// (ntexapi.h) — node ids fit in 6 bits, comfortably in `uint8_t`. The
// sentinel `kNumaNodeUnassigned (0xFF)` marks logical-processor slots
// that the system did not enumerate (sparse processor-group population).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NUMA_TOPOLOGY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NUMA_TOPOLOGY_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Sentinel: cpu_to_node[i] == 0xFF means "no NUMA node enumerated for
// this logical-processor slot". Treated as node 0 by callers that need a
// concrete node id; callers that care about presence test against this
// constant directly.
inline constexpr uint8_t kNumaNodeUnassigned = 0xFF;

// Capacity of the flat cpu→node table. NT processor groups are 64 LPs
// each and currently capped at 4 active groups (PEB-level invariant on
// every supported Windows build), so 4 × 64 = 256 covers every logical
// processor a libc-managed thread can land on. Direct-addressable by
// `Group * 64 + Number` from `PROCESSOR_NUMBER`.
inline constexpr uint32_t kNumaCpuTableSize = 256;

// Process-lifetime NUMA snapshot. Stored inline in `PcbZone0`; sealed
// PAGE_READONLY end of Tier A.
//
// `populated` is the only field that distinguishes "topology probe ran
// and succeeded" from "probe ran and failed / not yet run". A failed
// probe leaves `single_node = 1`, `highest_node = 0`, and
// `cpu_to_node[]` all-zero — every caller still resolves to a valid
// node id, just one without per-node replication. This is the safe
// degradation: behaviour matches a single-node system, which is the
// overwhelming common case anyway.
struct alignas(8) NumaTopology {
  // Highest enumerated NUMA node id (clamped to 63). On a single-node
  // box this is 0.
  uint8_t highest_node;

  // 1 iff `highest_node == 0`. Pre-computed so the partition selector
  // hot path is `if (topo.single_node != 0) return kNodeAgnostic;` —
  // one byte load + branch, no arithmetic. Stored as `uint8_t` rather
  // than `bool` to keep the struct's exact 264 B layout obvious to
  // the layout `static_assert` in PCB Zone 0.
  uint8_t single_node;

  // 1 iff `populate_from_system()` succeeded. Allows downstream callers
  // (numa_ops, sched_ops) to distinguish "no NUMA hardware" from
  // "topology query failed". Both behave identically for the partition
  // selector, but `mbind` / `getcpu` care about the difference.
  uint8_t populated;

  // Number of distinct processor groups that contained at least one
  // enumerated NUMA node. 1 on every desktop / single-socket workstation.
  uint8_t group_count;

  uint32_t _reserved0;

  // Direct-addressable cpu→node table. Index = `Group * 64 + Number`.
  // Entries default to `kNumaNodeUnassigned` until the probe writes
  // them. Built from the variable-length `NUMA_NODE_RELATIONSHIP` list
  // by iterating the set bits of each entry's `GroupMask.Mask`.
  uint8_t cpu_to_node[kNumaCpuTableSize];
};

static_assert(sizeof(NumaTopology) == 264,
              "NumaTopology layout must be 8 B header + 256 B table");

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NUMA_TOPOLOGY_H
