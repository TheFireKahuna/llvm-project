//===---------- Windows NUMA ops engine (set/get_mempolicy, mbind) ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine functions for NUMA memory policy operations. Each returns 0 on
// success or -errno on failure. The wrapper/entry-point layers translate
// to ErrorOr<int> and libc_errno respectively.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/legacy/numa_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/memory/legacy/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/remap_guard.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/legacy/numa_policy.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_snapshot.h"
#include "src/__support/OSUtil/windows/memory/legacy/view_spec.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt_pal/numa_topology.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h> // uint64_t, UINT64_C

namespace LIBC_NAMESPACE_DECL {

namespace {

/// Read a multi-word unsigned long nodemask into a uint64_t.
/// On Windows LLP64, unsigned long is 32 bits, so nodes 32-63 live in
/// nodemask[1].
uint64_t read_nodemask(const unsigned long *nodemask,
                                   unsigned long maxnode) {
  uint64_t mask = static_cast<uint64_t>(nodemask[0]);
  if (maxnode > 32)
    mask |= static_cast<uint64_t>(nodemask[1]) << 32;
  return mask;
}

/// Write a uint64_t value into a multi-word unsigned long nodemask array.
/// On Windows LLP64, unsigned long is 32 bits, so nodes 32-63 require
/// writing to nodemask[1].
void write_nodemask(unsigned long *nodemask, unsigned long maxnode,
                                uint64_t value) {
  nodemask[0] = static_cast<unsigned long>(value & 0xFFFFFFFF);
  if (maxnode > 32)
    nodemask[1] = static_cast<unsigned long>(value >> 32);
}

/// Select the first set bit in a nodemask as the target NUMA node.
int first_set_node(uint64_t mask) {
  return __builtin_ctzll(mask);
}

// Flags we implement. Reject anything else to avoid silent success on
// unsupported operations (e.g. MPOL_MF_MOVE_ALL = 0x04 on Linux).
constexpr unsigned int SUPPORTED_FLAGS = MPOL_MF_STRICT | MPOL_MF_MOVE;

/// Verify per-page NUMA placement via batched WorkingSetExInformation.
/// Returns 0 if all resident pages are on nodes in mask, -EIO if any
/// are misplaced. Non-resident and reserved pages are skipped.
///
/// Uses bulk VA walk (NtPssCaptureVaSpaceBulk via nt_pal::RegionWalker) for the
/// outer region enumeration — 1.3-3.2x faster than iterative
/// NtQueryVirtualMemory (RA14/Frontier 2). The inner WSEx batched query
/// is unchanged (it queries per-page NUMA node, not region state).
int verify_numa_placement(HANDLE process, char *start,
                                       char *end, uint64_t mask) {
  const SIZE_T page_size = windows::get_page_size();
  constexpr SIZE_T WS_BATCH = 256;
  MEMORY_WORKING_SET_EX_INFORMATION ws_info[WS_BATCH];

  SIZE_T range_size = static_cast<SIZE_T>(end - start);
  auto ws = windows::byte_scratch(4096);
  if (!ws) return -ENOMEM;
  nt_pal::RegionWalker walk(start, range_size, ws.data(), ws.size());

  while (walk.next()) {
    if (walk.entry->State == MEM_FREE)
      return -EFAULT;

    if (walk.entry->State != MEM_COMMIT)
      continue;

    char *cur = static_cast<char *>(walk.chunk);
    SIZE_T region_pages = walk.chunk_size / page_size;

    for (SIZE_T i = 0; i < region_pages; i += WS_BATCH) {
      SIZE_T batch = region_pages - i;
      if (batch > WS_BATCH)
        batch = WS_BATCH;

      char *batch_addr = cur + i * page_size;
      for (SIZE_T j = 0; j < batch; ++j) {
        ws_info[j].VirtualAddress = batch_addr;
        ws_info[j].VirtualAttributes.Flags = 0;
        batch_addr += page_size;
      }
      NTSTATUS ws_status = ::NtQueryVirtualMemory(
          process, nullptr, MemoryWorkingSetExInformation, ws_info,
          batch * sizeof(ws_info[0]), nullptr);
      if (NT_ERROR(ws_status))
        return -EIO;

      for (SIZE_T j = 0; j < batch; ++j) {
        if (!ws_info[j].VirtualAttributes.Valid)
          continue;
        ULONG node =
            static_cast<ULONG>(ws_info[j].VirtualAttributes.Node);
        if (node < 64 && !(mask & (UINT64_C(1) << node)))
          return -EIO;
      }
    }
  }
  return 0;
}

// InterleaveRotation removed — NUMA interleave is now handled by the VEH
// demand-commit handler via VM_FLAG_NUMA_INTERLEAVE on the mapping table slot.

/// Rebind a section view to a NUMA node (or interleave across nodes).
///
/// NUMA migration is unmap -> remap with a NUMA hint. The section preserves
/// content through the cycle. COW'd pages on MAP_PRIVATE file views are
/// saved/restored via the placeholder-preserving COW protocol.
///
/// MPOL_INTERLEAVE: instead of splitting into N page-sized views (75K
/// syscalls for 100MB), remap as a single SEC_RESERVE view and set the
/// VM_FLAG_NUMA_INTERLEAVE flag. The VEH demand-commit handler commits
/// each page on first access with a deterministic NUMA node rotation.
/// Cost: 2 syscalls upfront + 1 per page on fault (matches Linux).
///
/// Returns 0 on success, -errno on failure.

/// Rebind a section view to a NUMA node (or interleave across nodes).
///
/// Uses RemapGuard for RAII rollback: if any remap fails, the guard
/// destructor re-remaps the original view, restores COW pages, and
/// releases the mapping table lock.
long rebind_region(HANDLE process, char *start, SIZE_T size,
                               DWORD /*orig_protect*/, ULONG target_node,
                               uint64_t interleave_mask = 0) {
  const bool is_interleave = interleave_mask != 0;
  uintptr_t base_val = reinterpret_cast<uintptr_t>(start);

  windows::RemapGuard guard(start, size);
  if (!guard.prepare() || guard.section_handle() == nullptr)
    return -EINVAL;

  if (!guard.unmap())
    return -ENOMEM; // ~guard remaps original view.

  const auto &entry = guard.entry();
  windows::memory::RegionDesc *region = guard.region();

  // Build a transient ViewSpec for the section-borrow remap helpers.
  windows::ViewSpec spec{};
  spec.section = region->section_handle;
  spec.file = region->file_handle;
  spec.offset = region->section_offset;
  spec.prot = entry.view_prot;
  spec.flags = entry.flags;

  NTSTATUS st;
  bool ok = false;
  DWORD new_flags = entry.flags;

  if (is_interleave) {
    // Remap as SEC_RESERVE — VEH demand-commits pages with NUMA rotation.
    st = spec.map_into_reserve(start, size);
    ok = NT_SUCCESS(st);

    if (ok) {
      guard.cow().restore(base_val, guard.records(), guard.record_count());
      new_flags |= windows::VM_FLAG_NUMA_INTERLEAVE;
    }
  } else {
    // Single-node: remap with MemExtendedParameterNumaNode.
    MEM_EXTENDED_PARAMETER param = {};
    param.Type = MemExtendedParameterNumaNode;
    param.ULong = target_node;

    st = spec.map_into_ex(start, size, &param, 1);
    if (NT_ERROR(st)) {
      // Fallback: remap without NUMA to avoid losing the mapping.
      st = spec.map_into(start, size);
    }
    ok = NT_SUCCESS(st);

    if (ok) {
      guard.cow().restore(base_val, guard.records(), guard.record_count());
      if (guard.record_count() > 0)
        windows::replay_protections(process, base_val, spec.prot,
                                    guard.records(), guard.record_count());
    }
  }

  if (!ok)
    return -ENOMEM; // ~guard rolls back.

  if (!guard.commit(start, size, entry.region_id, entry.alloc_id, spec.prot,
                    new_flags))
    return -ENOMEM; // ~guard rolls back.

  if (is_interleave)
    windows::g_mapping_table.set_numa_interleave(start, interleave_mask);

  return 0;
}

} // namespace

namespace internal {

intptr_t set_mempolicy(int mode, const unsigned long *nodemask,
                   unsigned long maxnode) {
  if (mode < MPOL_DEFAULT || mode > MPOL_INTERLEAVE) {
    return -EINVAL;
  }

  // MPOL_DEFAULT ignores nodemask/maxnode.
  if (mode == MPOL_DEFAULT) {
    windows::NumaPolicy &policy = windows::get_thread_numa_policy();
    policy.mode = MPOL_DEFAULT;
    policy.nodemask = 0;
    policy.maxnode = 0;
    return 0;
  }

  if (!nodemask || maxnode == 0) {
    return -EINVAL;
  }

  // Sealed Zone 0 snapshot — built once at libc init from
  // NtQuerySystemInformationEx(SystemLogicalProcessorInformationEx,
  // RelationNumaNode, ...). On topology-probe failure `populated == 0`
  // and `highest_node == 0`, which the callers below handle as the
  // single-node degradation path.
  const windows::NumaTopology &topo = nt_pal::numa_topology();
  ULONG highest_node = static_cast<ULONG>(topo.highest_node);

  unsigned long effective_maxnode = maxnode;
  if (effective_maxnode > 64)
    effective_maxnode = 64;

  // Widen to uint64_t for safe bit manipulation on LLP64.
  uint64_t mask = read_nodemask(nodemask, effective_maxnode);

  // Only consider the first effective_maxnode bits.
  if (effective_maxnode < 64)
    mask &= (UINT64_C(1) << effective_maxnode) - 1;

  // Mask off bits beyond the highest available node.
  if (highest_node < 63)
    mask &= (UINT64_C(1) << (highest_node + 1)) - 1;

  if (mask == 0) {
    return -EINVAL;
  }

  windows::NumaPolicy &policy = windows::get_thread_numa_policy();
  policy.mode = mode;
  policy.nodemask = mask;
  policy.maxnode = effective_maxnode;
  return 0;
}

intptr_t get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode,
                   void *addr, unsigned long flags) {
  // Linux rejects MPOL_F_MEMS_ALLOWED combined with MPOL_F_ADDR or
  // MPOL_F_NODE.
  if ((flags & MPOL_F_MEMS_ALLOWED) &&
      (flags & (MPOL_F_ADDR | MPOL_F_NODE))) {
    return -EINVAL;
  }

  // MPOL_F_ADDR: query NUMA node for a specific address.
  if (flags & MPOL_F_ADDR) {
    if (!addr) {
      return -EINVAL;
    }

    windows::RegionInfo info;
    if (!windows::query_region_info(addr, &info)) {
      return -EFAULT;
    }

    if (flags & MPOL_F_NODE) {
      // Return the NUMA node ID in *mode, not the policy.
      if (mode)
        *mode = static_cast<int>(info.numa_node);
    } else {
      // Per-address policy is not tracked on Windows; report MPOL_DEFAULT.
      if (mode)
        *mode = MPOL_DEFAULT;
    }

    if (nodemask && maxnode > 0) {
      uint64_t node_bit = 0;
      if (info.numa_node < 64)
        node_bit = UINT64_C(1) << info.numa_node;
      write_nodemask(nodemask, maxnode, node_bit);
    }
    return 0;
  }

  // MPOL_F_MEMS_ALLOWED: return bitmask of all available NUMA nodes.
  if (flags & MPOL_F_MEMS_ALLOWED) {
    // Read from the sealed Zone 0 snapshot — no syscall on the
    // get_mempolicy hot path.
    ULONG highest_node =
        static_cast<ULONG>(nt_pal::numa_topology().highest_node);

    if (nodemask && maxnode > 0) {
      uint64_t allowed;
      if (highest_node >= 63)
        allowed = ~UINT64_C(0);
      else
        allowed = (UINT64_C(1) << (highest_node + 1)) - 1;
      write_nodemask(nodemask, maxnode, allowed);
    }

    // Linux ignores the mode argument for MPOL_F_MEMS_ALLOWED.
    return 0;
  }

  // MPOL_F_NODE without MPOL_F_ADDR: return the next interleave node.
  // Linux returns EINVAL for non-INTERLEAVE policies.
  if (flags & MPOL_F_NODE) {
    const windows::NumaPolicy &policy = windows::get_thread_numa_policy();
    if (policy.mode != MPOL_INTERLEAVE) {
      return -EINVAL;
    }
    if (mode)
      *mode = static_cast<int>(windows::select_numa_node());
    return 0;
  }

  // Default (flags=0): return the thread-local policy from set_mempolicy().
  // Linux returns EINVAL if addr is non-NULL without MPOL_F_ADDR.
  if (addr) {
    return -EINVAL;
  }

  const windows::NumaPolicy &policy = windows::get_thread_numa_policy();

  if (mode)
    *mode = policy.mode;

  if (nodemask && maxnode > 0)
    write_nodemask(nodemask, maxnode, policy.nodemask);

  return 0;
}

intptr_t mbind(void *addr, unsigned long len, int mode,
           const unsigned long *nodemask, unsigned long maxnode,
           unsigned int flags) {
  // Linux mbind(NULL, 0, ...) succeeds. Check len before addr.
  if (len == 0)
    return 0;

  if (LIBC_UNLIKELY(!addr || !windows::is_page_aligned(addr))) {
    return -EINVAL;
  }

  if (mode < MPOL_DEFAULT || mode > MPOL_INTERLEAVE) {
    return -EINVAL;
  }

  // Reject unsupported flags (e.g. MPOL_MF_MOVE_ALL).
  if (flags & ~SUPPORTED_FLAGS) {
    return -EINVAL;
  }

  const SIZE_T rounded_len = windows::round_to_page(len);
  if (LIBC_UNLIKELY(rounded_len == 0)) {
    // round_to_page returns 0 on overflow.
    return -EINVAL;
  }

  // Build a uint64_t nodemask from the caller's unsigned long array.
  // On Windows LLP64, unsigned long is 32 bits; use uint64_t internally
  // to support up to 64 NUMA nodes without undefined behavior.
  uint64_t mask = 0;
  if (nodemask && maxnode > 0) {
    mask = read_nodemask(nodemask, maxnode);
    // Clamp to nodes that actually exist — read from the sealed
    // Zone 0 snapshot.
    ULONG highest_node =
        static_cast<ULONG>(nt_pal::numa_topology().highest_node);
    if (highest_node < 63)
      mask &= (UINT64_C(1) << (highest_node + 1)) - 1;
  }

  // MPOL_DEFAULT requires an empty nodemask. Linux returns EINVAL if any
  // bits are set.
  if (mode == MPOL_DEFAULT) {
    if (mask != 0) {
      return -EINVAL;
    }
    // MPOL_DEFAULT means "use system default." With MPOL_MF_MOVE this is
    // semantically a no-op — the system already chose placement. Migrating
    // to an arbitrary node would be incorrect.
    return 0;
  }

  // Non-default modes require at least one valid node.
  if (mask == 0) {
    return -EINVAL;
  }

  // For MPOL_INTERLEAVE with MPOL_MF_MOVE, per-page round-robin is applied
  // in rebind_region (pages are recommitted one at a time to rotating nodes).
  // For non-MOVE cases, use the first set node as a single target.
  const bool interleave = (mode == MPOL_INTERLEAVE);
  ULONG target_node = static_cast<ULONG>(first_set_node(mask));

  HANDLE process = NtCurrentProcess();
  char *current = static_cast<char *>(addr);
  char *end = current + rounded_len;

  // MPOL_MF_STRICT without MPOL_MF_MOVE: verify per-page NUMA placement.
  // Uses actual physical node from WorkingSetExInformation, not the
  // allocation-time hint. Non-resident pages are skipped.
  if ((flags & MPOL_MF_STRICT) && !(flags & MPOL_MF_MOVE))
    return verify_numa_placement(process, current, end, mask);

  // MPOL_MF_MOVE: migrate pages to the target NUMA node via section remap.
  //
  // Under the section-backed model, all mappings (anonymous and file-backed)
  // are section views. Migration is unmap -> remap with NUMA hint. The section
  // preserves content — no save buffer, no memcpy.
  //
  // For MPOL_INTERLEAVE: the placeholder is split into page-sized chunks,
  // each remapped with a rotating NUMA node, matching Linux's per-page
  // NUMA interleave.
  //
  // The VEH remap guard stalls faulting threads during the unmap -> remap
  // window, matching Linux's mmap_lock serialization during migration.
  if (flags & MPOL_MF_MOVE) {
    // Bulk MBI via nt_pal::RegionWalker; rebind_region preserves VA layout at the
    // same AllocationBase (unmap+remap targets the same start/size).
    auto ws = windows::byte_scratch(4096);
    if (!ws)
      return -ENOMEM;
    nt_pal::RegionWalker walk(current, static_cast<SIZE_T>(end - current),
                               ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_COMMIT &&
          walk.entry->Type == MEM_MAPPED) {
        long rc = rebind_region(process,
                                static_cast<char *>(walk.entry->AllocationBase),
                                walk.chunk_size, walk.entry->Protect,
                                target_node, interleave ? mask : 0);
        if (rc < 0)
          return rc;
      }
    }

    // MPOL_MF_STRICT | MPOL_MF_MOVE: verify post-migration placement.
    // Windows NUMA hints are advisory — the kernel may have placed pages
    // on a different node than requested.
    if (flags & MPOL_MF_STRICT) {
      char *verify = static_cast<char *>(addr);
      char *verify_end = verify + rounded_len;
      int result = verify_numa_placement(process, verify, verify_end, mask);
      if (result != 0)
        return result;
    }

    return 0;
  }

  // No action flags: no-op. Windows cannot retroactively set NUMA policy
  // on committed memory without MPOL_MF_MOVE.
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
