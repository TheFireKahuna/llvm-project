//===-- PcbInitAccess — Zone 0 write gate ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PcbInitAccess is the sole class that can write PcbZone0 private fields.
// It is friended by PcbZone0 and provides static setter methods.
//
// Include this header ONLY in:
//   - startup init functions (app_init.cpp, process_identity.cpp)
//   - fork reinit functions (process_control_block.cpp)
//
// Subsystem code that only reads Zone 0 should use the public const
// accessors on PcbZone0 (g_pcb.zone0.page_size(), etc.) and should
// NOT include this header.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PCB_INIT_ACCESS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PCB_INIT_ACCESS_H

#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Compile-time gate is the friend relationship; this runtime gate is
// belt-and-suspenders for debug builds. Zone 0 setters must only fire
// before Tier A publishes (i.e. before pcb_init_state_advance(TierA));
// Zone 0b setters must additionally be inside a known unseal window
// post-Tier-A. Stray callers fail LIBC_ASSERT in debug; release builds
// rely on the hardware seal to AV the actual write.
#define LIBC_PCB_ASSERT_TIER_A_OPEN()                                          \
  LIBC_ASSERT(pcb_init_state() < PcbInitState::TierA &&                        \
              "PcbInitAccess Zone 0 setter called after Tier A seal — "        \
              "Zone 0 is sealed PAGE_READONLY for process lifetime. "          \
              "Any TierA_* intermediate state is permitted; TierA (sealed) "   \
              "and TierB are not.")

#define LIBC_PCB_ASSERT_ZONE0B_OPEN()                                          \
  LIBC_ASSERT(zone0b_writable_now() &&                                         \
              "PcbInitAccess Zone 0b setter called outside of "                \
              "pcb_unseal_readonly_b()/pcb_seal_readonly_b() pair")

class PcbInitAccess {
public:
  // ---------------------------------------------------------------------
  // Zone 0 setters (lifetime-immutable). Callable only during Tier A
  // bring-up — Zone 0 is sealed PAGE_READONLY at end of Tier A and is
  // never unsealed.
  // ---------------------------------------------------------------------

  LIBC_INLINE static void set_page_size(uint32_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.page_size_ = v;
  }
  LIBC_INLINE static void set_alloc_granularity(uint32_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.alloc_granularity_ = v;
  }
  LIBC_INLINE static void set_min_address(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.min_address_ = v;
  }
  LIBC_INLINE static void set_max_address(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.max_address_ = v;
  }
  LIBC_INLINE static void set_module_handle(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.module_handle_ = v;
  }
  LIBC_INLINE static void set_dso_handle(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.dso_handle_ = v;
  }
  LIBC_INLINE static void set_session_id(uint32_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.session_id_ = v;
  }
  LIBC_INLINE static void set_nt_build(uint32_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.nt_build_ = v;
  }
  LIBC_INLINE static void set_capabilities(uint32_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.capabilities_ = v;
  }
  LIBC_INLINE static NtOptionalSyscalls &optional_mut() {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    return g_pcb.zone0.optional_;
  }
  // Sealed VEH state mutator — used by the .libcveh sweep during Tier A
  // before Zone 0 seal. Returns a writable reference; the sweep populates
  // filters[] / filter_count and the TLS index allocators store into the
  // index fields.
  LIBC_INLINE static windows::VehSealedState &veh_sealed_mut() {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    return g_pcb.zone0.veh_sealed_;
  }

  // Mapping table sealed handles — written exactly once by
  // MappingTable::ensure_init() during Tier A Phase 0c.5. Kept in Zone 0
  // (lifetime-immutable) because the table itself is never re-initialised
  // post-bootstrap: fork inherits via CoW, exec preserves the VAs, and
  // process fini just lets the seal die with the process.
  LIBC_INLINE static void set_mapping_table_max_view_base(uintptr_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.mapping_table_max_view_base_ = v;
  }
  LIBC_INLINE static void set_mapping_table_l1_size(size_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.mapping_table_l1_size_ = v;
  }
  LIBC_INLINE static void set_mapping_table_l1(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.mapping_table_l1_ = v;
  }
  LIBC_INLINE static void set_mapping_table_remap_guards(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.mapping_table_remap_guards_ = v;
  }

  // VaSubstrate sealed state — populated by VaSubstrate::pre_init during
  // Tier A Phase 0a (before any allocator is up). All three values are
  // lifetime-immutable after the Tier A seal. The secrets are
  // ProcessPrng-seeded and guaranteed non-zero via init_seed_or_trap.
  LIBC_INLINE static void set_substrate_secret(uintptr_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.substrate_secret_ = v;
  }
  LIBC_INLINE static void set_substrate_token_key(uintptr_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.substrate_token_key_ = v;
  }
  LIBC_INLINE static void set_substrate_root(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.substrate_root_ = v;
  }

  // Pagemap sealed handles — populated by `pagemap_init_fn` during
  // Tier A Phase 3. All three values are lifetime-immutable after the
  // Tier A seal. The reservation stays committed PAGE_READONLY for the
  // process lifetime (snmalloc `notify_using_readonly` pattern); the
  // kernel mutates PTE state on `nt_pal::protect` upgrades but never
  // touches the [base, end) bounds or the cookie.
  LIBC_INLINE static void set_pagemap_base(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.pagemap_base_ = v;
  }
  LIBC_INLINE static void set_pagemap_end(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.pagemap_end_ = v;
  }
  LIBC_INLINE static void set_pagemap_cookie(uintptr_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.pagemap_cookie_ = v;
  }

  // Buddy arena sealed handles — populated by `buddy_arena_init_fn` during
  // Tier A Phase 4 (after pagemap is online so the buddy's three
  // reservations can be marked LIBC_INTERNAL via the Receipt mechanism).
  // All six values are lifetime-immutable after the Tier A seal; chunk
  // alloc/free mutates the per-arena BSS state (live_count, generation,
  // tree_init latch) but never the sealed bases / capacity / secret.
  LIBC_INLINE static void set_buddy_partition_base(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.buddy_partition_base_ = v;
  }
  LIBC_INLINE static void set_buddy_partition_bytes(size_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.buddy_partition_bytes_ = v;
  }
  LIBC_INLINE static void set_buddy_tree_base(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.buddy_tree_base_ = v;
  }
  LIBC_INLINE static void set_buddy_desc_pool_base(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.buddy_desc_pool_base_ = v;
  }
  LIBC_INLINE static void set_buddy_desc_pool_capacity(size_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.buddy_desc_pool_capacity_ = v;
  }
  LIBC_INLINE static void set_buddy_arena_secret(uintptr_t v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.buddy_arena_secret_ = v;
  }

  // Partition layer sealed handles — populated by `partition_init_fn`
  // during Tier A Phase 5 (after pagemap=3 and buddy_arena=4 are online so
  // the partition's three reservations can be marked LIBC_INTERNAL via
  // the Receipt mechanism). The three Zone 0 pointers are lifetime-
  // immutable after the Tier A seal; partition lifecycle (reserve_or_grow
  // / commit / decommit / retire) mutates the descriptor pool slots and
  // reserve table entries via atomic operations on file-scope bitmaps
  // and the inline atomic-pointer table — never the sealed bases.
  // The fork-mutable canary key (`partition_secret`) is a Zone 0b setter
  // — see further down.
  LIBC_INLINE static void set_partition_coarse_pagemap(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.partition_coarse_pagemap_ = v;
  }
  LIBC_INLINE static void set_partition_reserve_table(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.partition_reserve_table_ = v;
  }
  LIBC_INLINE static void set_partition_desc_pool_base(void *v) {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    g_pcb.zone0.partition_desc_pool_base_ = v;
  }

  // NUMA topology snapshot — populated by `pal_init_fn` during Tier A
  // Phase 0. Returns a writable reference; the probe walks the
  // `NtQuerySystemInformationEx(SystemLogicalProcessorInformationEx,
  // RelationNumaNode, ...)` result and stamps `cpu_to_node[]`,
  // `highest_node`, `single_node`, `group_count`, and `populated`
  // before the Tier A seal closes Zone 0 PAGE_READONLY for the rest of
  // the process lifetime.
  LIBC_INLINE static windows::NumaTopology &numa_topology_mut() {
    LIBC_PCB_ASSERT_TIER_A_OPEN();
    return g_pcb.zone0.numa_topology_;
  }

  // ---------------------------------------------------------------------
  // Zone 0b setters (fork-mutable). Callable during Tier A bring-up AND
  // inside libc_fork_reinit() / veh_core fini, between matching
  // pcb_unseal_readonly_b() / pcb_seal_readonly_b() calls.
  // ---------------------------------------------------------------------

  LIBC_INLINE static void set_pid(pid_t v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.pid_ = v;
  }
  LIBC_INLINE static void set_parent_pid(DWORD v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.parent_pid_ = v;
  }
  LIBC_INLINE static void set_security_cookie(uintptr_t v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.security_cookie_ = v;
  }
  LIBC_INLINE static void set_security_cookie_complement(uintptr_t v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.security_cookie_complement_ = v;
  }
  LIBC_INLINE static void set_dll_notify_cookie(void *v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.dll_notify_cookie_ = v;
  }
  // Layer 7 partition canary key. Drawn fresh at Tier A Phase 5
  // (`partition_init_fn`) and re-rolled at fork priority 38
  // (`partition_fork_reinit`) inside the Zone 0b unseal window so the
  // child's canaries cannot be replayed against the parent.
  LIBC_INLINE static void set_partition_secret(uintptr_t v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.partition_secret_ = v;
  }
  // Per-process cookie probed via NtQueryInformationProcess(ProcessCookie).
  // Written at libc init by `pal_init_fn` and re-read after fork by
  // `pal_fork_reinit_impl` inside the Zone 0b unseal window — the kernel
  // rerolls the cookie on `RtlCloneUserProcess`, so the child must
  // re-probe rather than inherit the parent's value.
  LIBC_INLINE static void set_process_cookie(uint32_t v) {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.process_cookie_ = v;
  }
  LIBC_INLINE static void init_canary() {
    LIBC_PCB_ASSERT_ZONE0B_OPEN();
    g_pcb.zone0b.zone_canary_ =
        g_pcb.zone0b.security_cookie_ ^ PCB_CANARY_MAGIC;
  }

};

#undef LIBC_PCB_ASSERT_TIER_A_OPEN
#undef LIBC_PCB_ASSERT_ZONE0B_OPEN

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PCB_INIT_ACCESS_H
