//===-- nt_pal::pal_init — Layer 0 PAL initialization -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Phase-0 (`.libcmem$P0`) bootstrap handler. Probes the per-process
// cookie and `SeLockMemoryPrivilege` once and stamps the values into
// the PCB-resident `internal::NtPalProcessState`. Cached state is then
// available to every later memory subsystem (substrate at $P1, mapping
// table at $P2, etc.).
//
// Returns 0 receipts — PAL state is per-process metadata, not VA the
// mapping table tracks.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/nt_pal/large_pages.h"
#include "src/__support/OSUtil/windows/nt_pal/numa_topology.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Probe the NUMA topology via
// `NtQuerySystemInformationEx(SystemLogicalProcessorInformationEx,
//                              RelationNumaNode, ...)` and stamp the
// result into `g_pcb.zone0.numa_topology_`. Called from
// `pal_probe_once()` during Tier A Phase 0.
//
// At Phase 0 the substrate / byte_scratch / heap are not yet online, so
// the result buffer is stack-allocated. `NUMA_NODE_RELATIONSHIP` entries
// are ~40 B each (NodeNumber + 18 B reserved + GroupCount + GroupMask);
// the design ceiling of 64 NUMA nodes fits comfortably in 4 KiB.
//
// On any failure (query failed, buffer too small, malformed entries) we
// leave the topology in its zero-initialised state, which decodes as
// `single_node = 0, populated = 0`. We then promote that to
// `single_node = 1, populated = 0` so the partition selector takes the
// short-circuit path — degrading to single-node behaviour is the safe
// default that matches the overwhelming common case.
LIBC_INLINE static void probe_numa_topology() {
  windows::NumaTopology &topo =
      ::LIBC_NAMESPACE::internal::PcbInitAccess::numa_topology_mut();

  // Default-fill `cpu_to_node[]` with the unassigned sentinel so a
  // sparse processor-group population is observable to consumers that
  // care about presence (vs. callers that just want a node id and treat
  // unassigned as 0).
  for (uint32_t i = 0; i < windows::kNumaCpuTableSize; ++i)
    topo.cpu_to_node[i] = windows::kNumaNodeUnassigned;
  topo.highest_node = 0;
  topo.single_node = 1;
  topo.populated = 0;
  topo.group_count = 1;

  // 4 KiB is enough for 64 NUMA nodes' worth of NUMA_NODE_RELATIONSHIP
  // entries plus the SLPI_EX header overhead. If the system reports
  // more we fall through to the populated=0 path; consumers behave as
  // if the box were single-node, which is the conservative degradation.
  alignas(8) char buf[4096];
  ULONG returned = 0;
  LOGICAL_PROCESSOR_RELATIONSHIP filter = RelationNumaNode;
  NTSTATUS st = ::NtQuerySystemInformationEx(
      SystemLogicalProcessorInformationEx, &filter, sizeof(filter), buf,
      sizeof(buf), &returned);
  if (!NT_SUCCESS(st) || returned == 0)
    return;

  // Walk the variable-length entries by `entry->Size`. Each NumaNode
  // entry has at most one `GroupMask` (the union shape uses
  // `GroupCount = 1` on every observed Windows build); we still iterate
  // `GroupCount` masks for safety against the Win11+ `GroupMasks[]`
  // multi-group form (24H2 cluster-on-die).
  constexpr ULONG kMinEntry = 8;  // Relationship (4) + Size (4)
  uint8_t observed_groups_mask = 0;
  uint8_t highest = 0;
  const char *cur = buf;
  const char *end = buf + returned;
  while (cur + kMinEntry <= end) {
    auto *entry =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(cur);
    if (entry->Size < kMinEntry || cur + entry->Size > end)
      break;
    if (entry->Relationship != RelationNumaNode) {
      cur += entry->Size;
      continue;
    }

    const NUMA_NODE_RELATIONSHIP &nn = entry->NumaNode;
    // Clamp to the table's representable range. Node ids ≥ 64 fall off
    // the design's documented ceiling and would not fit in `uint8_t`
    // without additional encoding; treat as a probe failure.
    if (nn.NodeNumber > 63) {
      cur += entry->Size;
      continue;
    }
    uint8_t node = static_cast<uint8_t>(nn.NodeNumber);
    if (node > highest)
      highest = node;

    // Group-count == 0 is not legal on any Windows version; defensive
    // guard against malformed kernel output. Single-mask is the
    // overwhelming common case (all desktops, 99%+ of servers).
    USHORT group_count = nn.GroupCount;
    if (group_count == 0)
      group_count = 1;

    for (USHORT g = 0; g < group_count; ++g) {
      const GROUP_AFFINITY &ga =
          (group_count == 1) ? nn.GroupMask : nn.GroupMasks[g];
      WORD group = ga.Group;
      if (group >= 4)
        continue; // outside the kNumaCpuTableSize budget
      observed_groups_mask |=
          static_cast<uint8_t>(1u << (group & 0x3));

      KAFFINITY mask = ga.Mask;
      while (mask != 0) {
        ULONG bit = __builtin_ctzll(mask);
        mask &= mask - 1;
        uint32_t idx = static_cast<uint32_t>(group) * 64u + bit;
        if (idx < windows::kNumaCpuTableSize)
          topo.cpu_to_node[idx] = node;
      }
    }
    cur += entry->Size;
  }

  topo.highest_node = highest;
  topo.single_node = (highest == 0) ? 1 : 0;
  topo.group_count = static_cast<uint8_t>(__builtin_popcount(observed_groups_mask));
  if (topo.group_count == 0)
    topo.group_count = 1;
  topo.populated = 1;
}

// One-time probe. Runs at phase $P0 from `memory_primitives_startup_init()`
// and again from `pal_fork_reinit` after `RtlCloneUserProcess`.
//
// The cookie is written through `PcbInitAccess::set_process_cookie` —
// during Tier A bring-up Zone 0b is still PAGE_READWRITE, and after fork
// the libc_fork_reinit walker holds Zone 0b unsealed across our priority
// (kForkPrioPal = 36). `large_pages_available` lives in Zone 1 and stays
// a plain atomic store. `numa_topology_` is Zone 0 (lifetime-immutable);
// fork inherits it via CoW unchanged — NT preserves NUMA layout across
// `RtlCloneUserProcess` — so the topology probe runs only on the Tier A
// bring-up path, never on `pal_fork_reinit_impl`.
LIBC_INLINE static void pal_probe_once(bool topology) {
  // Probe the per-process cookie via NtQueryInformationProcess(36).
  ULONG cookie = 0;
  ULONG ret_len = 0;
  NTSTATUS st = ::NtQueryInformationProcess(NtCurrentProcess(), ProcessCookie,
                                             &cookie, sizeof(cookie),
                                             &ret_len);
  // On a healthy process the cookie is a non-zero ULONG. On failure
  // we leave the slot at zero — callers that check
  // `process_cookie() != 0` will see "not yet initialised" and treat
  // it as a fail-safe (e.g. don't xor-encode freelist pointers).
  if (NT_SUCCESS(st))
    ::LIBC_NAMESPACE::internal::PcbInitAccess::set_process_cookie(
        static_cast<uint32_t>(cookie));

  // Probe SeLockMemoryPrivilege. Probe-once; never per-allocation.
  g_pcb.nt_pal.large_pages_available.store(probe_se_lock_memory_privilege(),
                                            cpp::MemoryOrder::RELEASE);

  // NUMA topology only needs to be written during the Tier A bring-up
  // pass. Zone 0 is sealed PAGE_READONLY after Tier A, so a re-probe
  // here on the fork path would AV; the snapshot is fork-stable
  // anyway.
  if (topology)
    probe_numa_topology();
}

extern "C" uint32_t pal_init_fn(::LIBC_NAMESPACE::internal::Receipt * /*out*/,
                                uint32_t /*cap*/) {
  pal_probe_once(/*topology=*/true);
  return 0; // PAL has no VA regions for the mapping table to stamp.
}

// Re-probe after fork. Cookie is rerolled by the kernel on
// RtlCloneUserProcess; privilege state should survive but the re-probe
// is cheap. NUMA topology is fork-stable and lives in sealed Zone 0,
// so the topology pass is intentionally skipped here — Zone 0 is
// PAGE_READONLY in the child by the time fork-reinit walks the priority
// chain, and rewriting it would AV.
extern "C" void pal_fork_reinit_impl() { pal_probe_once(/*topology=*/false); }

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_MEMORY_PRIMITIVE(pal, 0,
                               &::LIBC_NAMESPACE::nt_pal::pal_init_fn)
