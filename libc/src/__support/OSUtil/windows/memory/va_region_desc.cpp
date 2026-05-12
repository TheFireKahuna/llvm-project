//===- va_region_desc.cpp - va_tracker Layer 1 leaf descriptor ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owns the `VaTrackerRegionDesc` partition's per-class chunk table, the
// allocator path, the FreeFn (`region_desc_release`, metadata-only), the
// clone-for-fragment helper, and the RegionDesc fork-reinit phase.
//
// Crystalline retirement of the descriptor itself rides
// `g_va_tracker_skiplist_domain`: the owning skiplist node retires the desc
// through the same domain it uses for its own retirement. There is no
// separate RegionDesc Crystalline domain.
//
// Kernel-state teardown lives elsewhere
// (`va_tracker_transaction.cpp::backing_kill_and_retire`); the FreeFn in
// this TU is pure metadata cleanup, per the discipline that Crystalline-W is
// asynchronous by construction and offers no synchronous grace primitive
// (Nikolaev & Ravindran, "Crystalline: Fast and Memory Efficient Wait-Free
// Reclamation," PLDI 2024, §1).
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
// trailing 48 KiB is committed-but-unused — bounded commit-charge cost, pages
// stay zero-backed until first touch.
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
              "RegionDesc chunk_bytes must be a multiple of pagemap "
              "stamping granularity");
static_assert(kRegionDescSlotSize >= sizeof(RegionDesc),
              "RegionDesc slot must hold a full RegionDesc object");

namespace {

struct alignas(64) PerRegionDescState {
  cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerBucket]{};
  alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
};

PerRegionDescState g_region_desc_state;

LIBC_INLINE uint64_t partition_secret() {
  return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Per-slot initializer invoked from the va_chunk acquire path. Stamps the
// node canary against the VaTrackerRegionDesc class id; no Crystalline
// init_node here because the descriptor retires through
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

// Two-phase acquire: hand the shared va_chunk allocator a spec describing the
// partition class and slot geometry, then loop — pull a slot from any live
// chunk if one is available, otherwise commit a fresh chunk and retry. A null
// return from `commit_new_va_chunk_for` is the only terminal failure.
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

// Recover (chunk_id, slot_idx) from the desc VA, triple-validate (bounds →
// per-slot canary → per-chunk canary), zero the slot, and return it to the
// pool. The desc carries no kernel handles — those live on the shared
// `DescBacking` referenced via `desc->backing_ref` and were already torn
// down synchronously by the Transaction whose commit retired this desc.
//
// Canary order matters: the per-slot canary is validated BEFORE the
// per-chunk canary, because the per-slot canary is the harder target for an
// attacker (per-slot entropy from `slot_idx`), and we want a forgery attempt
// to trap on the strongest check first.
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

  // Metadata-only cleanup. Kernel state was torn down at Transaction commit
  // via the backing's synchronous teardown; here we only zero the slot and
  // return it to the partition pool.
  __builtin_memset(static_cast<void *>(desc), 0, sizeof(RegionDesc));

  release_slot_in_va_chunk(rec.cd, rec.slot_idx, p.chunk_table);
}

//===----------------------------------------------------------------------===//
//  Fragment clone
//===----------------------------------------------------------------------===//

// Produce a fresh desc for a fragment of `src`. Shape, flags, view_prot, and
// numa_interleave_mask copy verbatim; section_offset shifts by the byte
// distance from the source low VA to the fragment low VA. The encoded
// BackingRef is shared, so the kernel placeholder is split zero times and
// the kernel handles are duplicated zero times.
//
// A failure path that discards this clone before publication leaks no
// kernel state: the backing FreeFn is metadata-only, and any backing
// teardown decision is the surviving Transaction's responsibility.
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

// Per-slot fork canary refresh callback. The shared
// `refresh_slot_canaries_in_chunk` walks the occupancy bitmap and
// dispatches here with the slot VA and pre-computed canary. RegionDesc
// carries no additional per-slot post-fork repair beyond the canary
// write.
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
