//===- va_region_desc.cpp - va_tracker Layer 1 leaf descriptor ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owns the RegionDesc partition's chunk table, allocator, metadata-only
// FreeFn, fragment clone, and fork-reinit phase. Descriptor retirement
// rides g_va_tracker_skiplist_domain (no separate RegionDesc domain);
// kernel-state teardown is run synchronously on the mutator path by
// va_tracker_execute.cpp::backing_kill_and_retire (invoked from
// reap_old_backings / rollback_provisional) — Crystalline-W is
// asynchronous by construction and offers no synchronous grace primitive
// (Nikolaev & Ravindran, PLDI 2024, §1).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/va_region_desc.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

namespace partition_ns = alloc::partition;

//===----------------------------------------------------------------------===//
//  RegionDesc partition geometry
//===----------------------------------------------------------------------===//

// 64 KiB chunk for pagemap alignment; 256 slots × 64 B = 16 KiB used. The
// trailing 48 KiB is committed-but-unused — bounded charge, pages stay
// zero-backed until first touch.
constexpr uint32_t kRegionDescSlotSize       = sizeof(RegionDesc); // 64
constexpr uint32_t kRegionDescSlotsPerChunk  = kSlotsPerChunk;     // 256
constexpr uint32_t kRegionDescChunkBytes     = 64u * 1024u;

static_assert(kRegionDescChunkBytes == 64u * 1024u,
              "RegionDesc chunk must be exactly 64 KiB");
static_assert(kRegionDescSlotSize * kRegionDescSlotsPerChunk <=
                  kRegionDescChunkBytes,
              "RegionDesc slot range overflows chunk");
static_assert(kRegionDescChunkBytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "chunk_bytes must be a multiple of pagemap granularity");
static_assert(kRegionDescSlotSize >= sizeof(RegionDesc),
              "slot must hold a full RegionDesc");

namespace {

struct alignas(64) PerRegionDescState {
  cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerBucket]{};
  alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
};

PerRegionDescState g_region_desc_state;

LIBC_INLINE uint64_t partition_secret() {
  return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// No Crystalline init_node here: the descriptor retires through
// g_va_tracker_skiplist_domain with the stamp happening at publish time.
void region_desc_init_slot(void *slot, VaChunkDesc * /*cd*/,
                           uint32_t chunk_id, uint32_t slot_idx,
                           void * /*ctx*/) {
  auto *rd = static_cast<RegionDesc *>(slot);
  rd->node_canary = compute_va_node_canary(
      partition_secret(),
      static_cast<uint16_t>(partition_ns::PartitionClass::VaTrackerRegionDesc),
      static_cast<uint8_t>(chunk_id), static_cast<uint8_t>(slot_idx));
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
//  Allocation
//===----------------------------------------------------------------------===//

// Two-phase acquire: pull a slot from any live chunk, otherwise commit a
// fresh chunk and retry. A null return from commit_new_va_chunk_for is the
// only terminal failure.
RegionDesc *region_desc_alloc() {
  PerRegionDescState &p = g_region_desc_state;

  VaChunkAcquireSpec spec{
      /*cls=*/partition_ns::PartitionClass::VaTrackerRegionDesc,
      /*chunk_table=*/p.chunk_table,
      /*next_chunk_id_hint=*/&p.next_chunk_id_hint,
      /*chunk_count=*/kChunksPerBucket,
      /*slots_per_chunk=*/kRegionDescSlotsPerChunk,
      /*consumer_bucket_id=*/kPoolBucketRegionDesc,
      /*init=*/&region_desc_init_slot,
      /*init_ctx=*/nullptr,
  };

  for (;;) {
    void *slot = va_chunk_acquire_slot(spec);
    if (slot != nullptr)
      return static_cast<RegionDesc *>(slot);

    VaChunkDesc *cd = commit_new_va_chunk_for(
        partition_ns::PartitionClass::VaTrackerRegionDesc,
        p.chunk_table, &p.next_chunk_id_hint,
        /*bucket_id=*/kPoolBucketRegionDesc, kRegionDescSlotSize,
        kRegionDescSlotsPerChunk, kRegionDescChunkBytes);
    if (cd == nullptr)
      return nullptr;
  }
}

//===----------------------------------------------------------------------===//
//  FreeFn — metadata-only descriptor release
//===----------------------------------------------------------------------===//

// Canary order: per-slot BEFORE per-chunk. Per-slot has the higher entropy
// (slot_idx contributes), so a forgery attempt traps on the strongest check
// first. Kernel handles were torn down at Transaction commit via the
// backing's synchronous path — this FreeFn touches metadata only.
void region_desc_release(RegionDesc *desc) {
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();
  PerRegionDescState &p = g_region_desc_state;

  RecoveredSlot rec = recover_slot_from_va(
      desc, kRegionDescChunkBytes, kRegionDescSlotSize,
      kRegionDescSlotsPerChunk, kChunksPerBucket, p.chunk_table);

  validate_slot_canaries_or_trap(
      desc->node_canary, rec.cd,
      static_cast<uint16_t>(partition_ns::PartitionClass::VaTrackerRegionDesc),
      static_cast<uint8_t>(rec.chunk_id),
      static_cast<uint8_t>(rec.slot_idx), partition_secret());

  __builtin_memset(static_cast<void *>(desc), 0, sizeof(RegionDesc));

  release_slot_in_va_chunk(rec.cd, rec.slot_idx, p.chunk_table);
}

//===----------------------------------------------------------------------===//
//  Fragment clone
//===----------------------------------------------------------------------===//

// Shape, flags, view_prot, numa_interleave_mask copy verbatim; section_offset
// shifts by (frag_lo - src_lo). The encoded BackingRef is shared — the
// kernel placeholder is split zero times and the kernel handles are
// duplicated zero times. A discarded clone leaks no kernel state because
// the backing FreeFn is metadata-only and any backing teardown decision
// belongs to the surviving Transaction.
RegionDesc *clone_region_desc_for_fragment(RegionDesc *src, uintptr_t src_lo,
                                           uintptr_t frag_lo) {
  if (src == nullptr)
    return nullptr;

  RegionDesc *dst = region_desc_alloc();
  if (dst == nullptr)
    return nullptr;

  dst->section_offset = src->section_offset;
  dst->section_offset.QuadPart +=
      static_cast<int64_t>(frag_lo - src_lo);
  // ACQUIRE on src then RELEASE on dst: dst will be published via the
  // Transaction's Swap-CAS, and a subsequent reader pinning dst must see
  // the shape/flags written here.
  dst->shape.store(src->shape.load(cpp::MemoryOrder::ACQUIRE),
                   cpp::MemoryOrder::RELEASE);
  dst->flags.store(src->flags.load(cpp::MemoryOrder::ACQUIRE),
                   cpp::MemoryOrder::RELEASE);
  dst->view_prot = src->view_prot;
  dst->numa_interleave_mask = src->numa_interleave_mask;
  dst->backing_ref = src->backing_ref;
  return dst;
}

//===----------------------------------------------------------------------===//
//  Fork
//===----------------------------------------------------------------------===//

namespace {

// RegionDesc has no per-slot post-fork repair beyond the canary write.
void region_desc_per_slot_canary_refresh(void *slot_va,
                                         uint64_t fresh_canary) {
  auto *rd = static_cast<RegionDesc *>(slot_va);
  rd->node_canary = fresh_canary;
}

} // anonymous namespace

void region_desc_fork_reinit_phase(RegionDescForkChunkVisitor visit,
                                   void *ctx) {
  constexpr uint16_t kRegionDescClsId = static_cast<uint16_t>(
      partition_ns::PartitionClass::VaTrackerRegionDesc);
  for (uint32_t cid = 0; cid < kChunksPerBucket; ++cid) {
    VaChunkDesc *cd = g_region_desc_state.chunk_table[cid].load(
        cpp::MemoryOrder::ACQUIRE);
    if (cd == nullptr)
      continue;
    if (visit != nullptr)
      visit(cd, ctx);
    cd->chunk_canary = compute_va_chunk_canary(
        partition_secret(), kRegionDescClsId,
        static_cast<uint8_t>(cid));
    refresh_slot_canaries_in_chunk(
        cd, kRegionDescClsId, static_cast<uint8_t>(cid),
        kRegionDescSlotsPerChunk, partition_secret(),
        &region_desc_per_slot_canary_refresh);
  }
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
