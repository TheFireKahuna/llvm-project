//===-- Sealed VA publication chokepoint ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single-writer publication primitive for libc-internal sealed VA ranges.
// Collapses ad-hoc per-subsystem CAS sites into one chokepoint whose
// publication boundary is the linearization point every wait-free consumer
// acquire-loads through.
//
// Two structural properties at publish time:
//
//   1. Disjointness — every new range checked against every prior range;
//      overlap traps. NT's VAD layer already prevents kernel-level overlap,
//      but a future hint-driven NtAllocateVirtualMemoryEx into a known free
//      hole would bypass that — the inventory keeps its own check.
//
//   2. Recognizability — for ranges that opt in, every covered pagemap
//      entry is stamped (slot_idx=SealedKind, tag=LibcSealed). Stamp
//      stores are RELEASE; readers ACQUIRE-load in pagemap.h
//      (cross-TU pair) and observe the stamp atomically.
//
// Single-producer: publication runs serially during Tier A bring-up; the
// check loops need no synchronisation against each other, only against
// future readers (provided by the pagemap's per-entry ACQUIRE).
//
// Honeypot discipline: the transient sorted inventory lives in BSS and is
// wiped to zero by seal_time_assert_and_wipe() before the PCB Zone 0
// read-only seal applies. After seal there is no centralized base table
// for a partial-read primitive to harvest.
//
// Not reentrant; producer is single-threaded. Not safe from a VEH handler
// — publish performs NtProtectVirtualMemory upgrades via
// pagemap_register_range.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SEALED_VA_PUBLISHER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SEALED_VA_PUBLISHER_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

// Fine-grained category of a sealed VA range. The numeric value is packed
// into the pagemap entry's slot_idx field on eager-stamped kinds so one
// 8 B pagemap load recovers the kind without a Zone 0 indirection.
// Numeric values are part of the diagnostic ABI — do not renumber. Adding
// a kind: append at the end and update kind_registers_in_pagemap in the
// implementation.
enum class SealedKind : uint32_t {
  Pagemap = 0,                // Pagemap backing VA (no eager stamp).
  BuddyPartition = 1,         // 4 GiB buddy arena (no eager stamp).
  BuddyTree = 2,              // NBALLOC tree backing (eager stamp).
  BuddyDescPool = 3,          // BuddyChunkDescriptor pool (eager stamp).
  PartitionCoarsePagemap = 4, // 256 KiB coarse pagemap (eager stamp).
  PartitionDescPool = 5,      // PartitionDescriptor pool (eager stamp).
  Partition = 6,              // 4 GiB per-class partition (no eager stamp).
};

// Publish a sealed VA range to the pagemap and the inventory.
//
// Inventory insert runs before the pagemap stamp loop so a publish that
// traps on overlap leaves no partially stamped pagemap. `size` is rounded
// up to kPagemapChunkBytes for the stamp loop. `base` must be
// chunk-aligned; NT's 64 KiB allocation granularity guarantees this for
// every nt_pal::reserve_* return — the check catches future callers that
// source VA elsewhere.
//
// Trap conditions (fail-closed bring-up; no recovery path):
//   * PCB init state has reached PcbInitState::TierA or beyond — this is
//     a Tier A bring-up primitive; post-Tier A is a structural bug.
//   * The transient table has been wiped by seal_time_assert_and_wipe.
//   * base null, size zero, base not chunk-aligned.
//   * Range overlaps a previously published range.
//   * Table at kMaxSealedRanges capacity.
void publish_sealed_va_range(SealedKind kind, void *base, size_t size);

// End-of-bring-up validation pass plus honeypot wipe. Final scan over
// adjacent pairs in the sorted inventory catches invariant drift in
// insert_sorted_or_trap; then memsets the inventory to zero so a
// post-seal partial-read primitive lands on zeros. Zone 0 fields that
// record individual sealed bases remain reachable — this only removes the
// centralized aggregate.
//
// Safe to call exactly once; a second call traps. Must run before the
// read-only seal applies to PCB Zone 0.
void seal_time_assert_and_wipe();

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
