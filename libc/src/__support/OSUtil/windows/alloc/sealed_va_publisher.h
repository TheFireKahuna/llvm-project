//===-- Sealed VA publication chokepoint ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Unified single-writer publication primitive for libc-internal sealed VA
/// ranges. Replaces a collection of ad-hoc per-subsystem CAS sites with one
/// disciplined chokepoint whose publication boundary is the linearization
/// point every wait-free consumer acquire-loads through.
///
/// Two structural properties are enforced at publish time:
///
///   1. Disjointness. Every newly published range is checked against every
///      previously published range; any overlap traps. NT's VAD layer
///      already prevents kernel-level overlap, but the inventory carries
///      its own check so a future caller that bypasses the kernel
///      allocator (e.g. a hint-driven `NtAllocateVirtualMemoryEx` placed
///      into a known free hole) cannot silently shadow an existing range.
///
///   2. Recognizability through the pagemap. For ranges that opt in, the
///      publisher stamps every covered pagemap entry with
///      `(slot_idx = SealedKind, tag = LibcSealed)`. A wait-free reader
///      that resolves an arbitrary user pointer through
///      `pagemap_load_decoded` then sees a kind-tagged result for any
///      libc-internal address without consulting a side table. The stamp
///      writes are sequenced before the publisher returns to the caller,
///      and pagemap readers acquire-load the encoded word — the
///      publication boundary is the moment the stamp loop completes.
///
/// Single-producer invariant. Publication runs serially during libc
/// bring-up; there is exactly one producer thread and no concurrent
/// publishers. The check loops therefore need no synchronisation against
/// each other, only against future readers, which is provided by the
/// pagemap's per-entry acquire-load.
///
/// Honeypot discipline. The transient sorted inventory lives in BSS and
/// is wiped to zero by `seal_time_assert_and_wipe()` before the
/// process-wide read-only seal is applied. After seal there is no
/// centralized base table for a partial-read primitive to harvest;
/// runtime queries that need the same information consult either the
/// sealed read-only fields published into the process control block or
/// the pagemap entry's `LibcSealed` tag.
///
/// Reentrancy. Not reentrant; the producer is single-threaded.
/// Not safe to call from a VEH handler — the call path performs
/// `NtProtectVirtualMemory` upgrades through `pagemap_register_range`.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SEALED_VA_PUBLISHER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_SEALED_VA_PUBLISHER_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

/// Fine-grained category of a sealed VA range.
///
/// The numeric value is packed into the `slot_idx` field of an eager-
/// stamped pagemap entry so a single 8-byte pagemap load recovers the
/// kind without a Zone 0 indirection. Numeric values are part of the
/// diagnostic ABI; do not renumber. Adding a kind: append at the end
/// and update `kind_registers_in_pagemap` in the implementation.
enum class SealedKind : uint32_t {
  Pagemap = 0,                ///< Pagemap backing VA (no eager stamp).
  BuddyPartition = 1,         ///< 4 GiB buddy arena (no eager stamp).
  BuddyTree = 2,              ///< NBALLOC tree backing (eager stamp).
  BuddyDescPool = 3,          ///< BuddyChunkDescriptor pool (eager stamp).
  PartitionCoarsePagemap = 4, ///< 256 KiB coarse pagemap (eager stamp).
  PartitionDescPool = 5,      ///< PartitionDescriptor pool (eager stamp).
  Partition = 6,              ///< 4 GiB per-class partition (no eager stamp).
};

/// Publish a sealed VA range to the wait-free pagemap and the inventory.
///
/// On success the range is recorded in a sorted transient table and,
/// for kinds that opt in via `kind_registers_in_pagemap`, every covered
/// pagemap entry is stamped with `(slot_idx = kind, tag = LibcSealed)`.
/// The stamp loop runs after the disjointness check so a publish that
/// traps on overlap leaves no partially stamped pagemap state.
///
/// Trap conditions (fail-closed bring-up; no recovery path):
///   * Caller's PCB init state has already advanced past Tier A.
///   * The transient table has been wiped by `seal_time_assert_and_wipe`.
///   * `base` is null, `size` is zero, or `base` is not
///     `kPagemapChunkBytes`-aligned. NT's 64 KiB allocation granularity
///     guarantees alignment for every `nt_pal::reserve_*` return; the
///     check catches future callers that source VA elsewhere.
///   * The new range overlaps a previously published range.
///   * The table is at `kMaxSealedRanges` capacity.
///
/// \param kind Category of the range, used for pagemap stamp encoding.
/// \param base Aligned base address of the reservation.
/// \param size Size in bytes; rounded up to `kPagemapChunkBytes` for
///             the pagemap stamp loop.
void publish_sealed_va_range(SealedKind kind, void *base, size_t size);

/// End-of-bring-up validation pass plus inventory wipe.
///
/// Performs a final scan over adjacent pairs in the sorted inventory to
/// catch invariant drift in the sorted-insert path, then `memset`s the
/// inventory to zero so a post-seal partial-read primitive lands on
/// zeros instead of the live sealed-VA inventory. The Zone 0 fields
/// that record individual sealed bases remain reachable through their
/// existing indirections — wiping the inventory does not change the
/// disclosure surface of those.
///
/// Safe to call exactly once; a second call traps. Must be invoked
/// before the read-only seal is applied to PCB Zone 0.
void seal_time_assert_and_wipe();

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
