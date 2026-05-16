//===- desc_backing.cpp - Backing allocator + metadata FreeFn -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every path here is pure userspace — slot canary validation, bitmap
// clears, partition pool release. CI grep gate enforces zero NT primitive
// calls in this TU; kernel-state teardown lives in va_tracker_transaction
// per the mutator-owns-kernel-state discipline (Crystalline-W has no
// synchronous grace primitive — Nikolaev & Ravindran, PLDI 2024 §1).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/desc_backing.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/alloc/partition_class.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
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

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    DescBacking, &desc_backing_free, kBackingRetireFreq, kBackingMaxIdx>
    g_va_tracker_backing_domain;

namespace {

constexpr uint32_t kBackingSlotSize = sizeof(DescBacking);          // 64
constexpr uint32_t kBackingSlotsPerChunk = kMaxSlotsPerChunk;       // 256
// 64 KiB chunk for pagemap alignment. 256 * 64 B = 16 KiB payload leaves
// 48 KiB committed-but-unused — same bounded waste the RegionDesc pool
// accepts for the same reason.
constexpr uint32_t kBackingChunkBytes = 64u * 1024u;

static_assert(kBackingChunkBytes == 64u * 1024u, "chunk == 64 KiB");
static_assert(kBackingSlotSize * kBackingSlotsPerChunk <= kBackingChunkBytes,
              "slot range fits chunk");
static_assert(kBackingSlotSize >= sizeof(DescBacking), "slot holds object");

struct alignas(64) PerBackingState {
    cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerPoolBucket]{};
    alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
};

PerBackingState g_backing_state;

// init_registration() must run exactly once per domain per Crystalline-W
// contract; this flag deduplicates competing first-allocator races.
cpp::Atomic<uint32_t> g_backing_init_done{0};

LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Mix partition_secret (rotated every fork) with the slot coords so a fresh
// slot's generation is unguessable cross-fork. Bump 0 -> 1 so the encoded
// ref at the (0, 0) slot cannot collide with kBackingRefNull.
[[nodiscard]] LIBC_INLINE uint32_t
seed_generation_for_slot(uint16_t chunk_id, uint16_t slot_idx) {
    uint64_t s = partition_secret();
    uint32_t mixed = static_cast<uint32_t>((s ^ (s >> 32)) +
                                            (uint32_t{chunk_id} << 16) +
                                            uint32_t{slot_idx});
    return mixed == 0 ? 1u : mixed;
}

// va_chunk_acquire_slot init callback. Runs on a bitmap-claimed,
// memset-zeroed slot whose chunk/slot canaries have already been validated
// against the pool's class id.
void backing_init_slot(void *slot, VaChunkDesc * /*cd*/,
                       uint32_t chunk_id, uint32_t slot_idx,
                       void * /*ctx*/) {
    auto *b = static_cast<DescBacking *>(slot);
    b->generation.store(
        seed_generation_for_slot(static_cast<uint16_t>(chunk_id),
                                  static_cast<uint16_t>(slot_idx)),
        cpp::MemoryOrder::RELAXED);
    // Plain (non-atomic) coord writes: kChunksPerPoolBucket and
    // kSlotsPerChunk are both 256, so u8 fits; the cross-thread
    // happens-before comes from the Swap publish on pred->next[0].
    b->cached_chunk_id = static_cast<uint8_t>(chunk_id);
    b->cached_slot_idx = static_cast<uint8_t>(slot_idx);
    // Sole-owner state until the visitor's Swap publishes a referencing
    // desc; kernel-state atomics stay nullptr until backing_set_kernel_state.
    b->state.store(kBackingStateLive, cpp::MemoryOrder::RELAXED);
    b->node_canary = compute_va_node_canary(
        partition_secret(),
        static_cast<uint16_t>(
            partition_ns::PartitionClass::VaTrackerDescBacking),
        static_cast<uint8_t>(chunk_id), static_cast<uint8_t>(slot_idx));
    g_va_tracker_backing_domain.init_node(b);
}

} // namespace

//===----------------------------------------------------------------------===//
// BatchLinkCodec definitions
//===----------------------------------------------------------------------===//

} // namespace va_tracker
} // namespace windows

namespace concurrent {

uint32_t BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::DescBacking>::
    encode(CrystallineNode *n) noexcept {
    using Backing = ::LIBC_NAMESPACE::windows::va_tracker::DescBacking;
    auto *b = static_cast<Backing *>(n);
    // Cached coords are write-once at slot init; retirement is downstream
    // of the desc publish, so the unsynchronised read is safe.
    return 1u + ((static_cast<uint32_t>(b->cached_chunk_id) << 8) |
                  static_cast<uint32_t>(b->cached_slot_idx));
}

CrystallineNode *
BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::DescBacking>::decode(
    uint32_t code) noexcept {
    using Backing = ::LIBC_NAMESPACE::windows::va_tracker::DescBacking;
    uint32_t v = code - 1u;
    uint32_t chunk_id = (v >> 8) & 0xFFu;
    uint32_t slot_idx = v & 0xFFu;
    auto *cd = ::LIBC_NAMESPACE::windows::va_tracker::g_backing_state
                   .chunk_table[chunk_id]
                   .load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(cd == nullptr))
        __builtin_trap();
    auto *base = static_cast<char *>(cd->chunk_base);
    return reinterpret_cast<Backing *>(
        base +
        slot_idx *
            ::LIBC_NAMESPACE::windows::va_tracker::kBackingSlotSize);
}

} // namespace concurrent

namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
// backing_alloc / backing_set_kernel_state / deref_backing_raw
//===----------------------------------------------------------------------===//

// Two-phase acquire: try the shared per-chunk scaffold; on empty, commit
// a fresh chunk and retry. Only terminal failure is a null from
// commit_new_va_chunk_for.
DescBacking *backing_alloc() {
    PerBackingState &p = g_backing_state;

    VaChunkAcquireSpec spec{
        /*cls=*/partition_ns::PartitionClass::VaTrackerDescBacking,
        /*chunk_table=*/p.chunk_table,
        /*next_chunk_id_hint=*/&p.next_chunk_id_hint,
        /*chunk_count=*/kChunksPerPoolBucket,
        /*slots_per_chunk=*/kBackingSlotsPerChunk,
        /*consumer_bucket_id=*/kPoolBucketDescBacking,
        /*init=*/&backing_init_slot,
        /*init_ctx=*/nullptr,
    };

    for (;;) {
        void *slot = va_chunk_acquire_slot(spec);
        if (slot != nullptr)
            return static_cast<DescBacking *>(slot);

        VaChunkDesc *cd = commit_new_va_chunk_for(
            partition_ns::PartitionClass::VaTrackerDescBacking,
            p.chunk_table, &p.next_chunk_id_hint,
            /*bucket_id=*/kPoolBucketDescBacking,
            kBackingSlotSize, kBackingSlotsPerChunk, kBackingChunkBytes);
        if (cd == nullptr)
            return nullptr;
    }
}

void backing_set_kernel_state(DescBacking *backing,
                              void *placeholder_base,
                              uint32_t placeholder_pages,
                              BackingShape shape,
                              HANDLE section_handle,
                              HANDLE file_handle) {
    if (LIBC_UNLIKELY(backing == nullptr))
        __builtin_trap();
    // shape is plain; its publish happens-before the section_handle RELEASE
    // below — a reader that ACQUIREs section_handle observes shape. Handles
    // before base; reaper's null-store uses the same order so a "load
    // handle, close handle" reader never sees a closed handle paired with a
    // non-null base.
    backing->shape = shape;
    backing->section_handle.store(section_handle, cpp::MemoryOrder::RELEASE);
    backing->file_handle.store(file_handle, cpp::MemoryOrder::RELEASE);
    // placeholder_pages is plain; its publish happens-before the
    // placeholder_base RELEASE below — a reader that ACQUIREs
    // placeholder_base observes placeholder_pages.
    backing->placeholder_pages = placeholder_pages;
    backing->placeholder_base.store(placeholder_base,
                                     cpp::MemoryOrder::RELEASE);
}

DescBacking *deref_backing_raw(BackingRef ref) {
    if (ref == kBackingRefNull)
        return nullptr;

    uint16_t chunk_id = backing_ref_chunk_id(ref);
    uint16_t slot_idx = backing_ref_slot_idx(ref);
    uint32_t generation = backing_ref_generation(ref);

    // STAGE 1 — bounds.
    if (LIBC_UNLIKELY(chunk_id >= kChunksPerPoolBucket))
        __builtin_trap();
    if (LIBC_UNLIKELY(slot_idx >= kBackingSlotsPerChunk))
        __builtin_trap();

    VaChunkDesc *cd = g_backing_state.chunk_table[chunk_id].load(
        cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(cd == nullptr))
        __builtin_trap();

    // STAGE 2 — canaries. Chunk first catches chunk_id transposition /
    // partition mismatch; slot second catches per-slot tampering.
    uint64_t expected_chunk_canary = compute_va_chunk_canary(
        partition_secret(),
        static_cast<uint16_t>(
            partition_ns::PartitionClass::VaTrackerDescBacking),
        static_cast<uint8_t>(chunk_id));
    if (LIBC_UNLIKELY(cd->chunk_canary != expected_chunk_canary))
        __builtin_trap();

    uintptr_t base = reinterpret_cast<uintptr_t>(cd->chunk_base);
    DescBacking *b = reinterpret_cast<DescBacking *>(
        base + static_cast<size_t>(slot_idx) * cd->slot_size);

    uint64_t expected_node_canary = compute_va_node_canary(
        partition_secret(),
        static_cast<uint16_t>(
            partition_ns::PartitionClass::VaTrackerDescBacking),
        static_cast<uint8_t>(chunk_id),
        static_cast<uint8_t>(slot_idx));
    if (LIBC_UNLIKELY(b->node_canary != expected_node_canary))
        __builtin_trap();

    // STAGE 3 — generation. ACQUIRE pairs with the alloc-side RELEASE seed
    // store; a recycled slot carries a different generation and traps.
    uint32_t live_gen = b->generation.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(live_gen != generation))
        __builtin_trap();

    return b;
}

//===----------------------------------------------------------------------===//
// Cross-domain pin defence — anchor pins on the backing domain
//===----------------------------------------------------------------------===//

namespace {
// Pure thunk: only the era-stability convergence inside protect() matters,
// not the return. Safe to invoke from a foreign helper per the helping-
// protocol thunk contract (no captures, no side effects).
inline DescBacking *backing_anchor_thunk(void *) { return nullptr; }
struct BackingAnchorCtx {};
} // namespace

void anchor_backing_engine_pin() {
    BackingAnchorCtx ctx{};
    (void)g_va_tracker_backing_domain.protect<&backing_anchor_thunk>(
        ctx, BackingPinSlot::kEngineAnchor, /*parent=*/nullptr);
}

void anchor_backing_reader_pin() {
    BackingAnchorCtx ctx{};
    (void)g_va_tracker_backing_domain.protect<&backing_anchor_thunk>(
        ctx, BackingPinSlot::kReaderPin, /*parent=*/nullptr);
}

//===----------------------------------------------------------------------===//
// Metadata FreeFn
//===----------------------------------------------------------------------===//

// Body cleanup only. Every kernel resource was torn down synchronously by
// backing_kill_and_retire before Crystalline grace expired and this fired;
// no nt_pal::* or NtClose may appear here. Triple-validates (canary first
// to defend against heap-spray), zero-fills the slot, and returns it via
// the chunk-state-machine drain — which may flip the chunk to Draining if
// this was the last live slot, retiring the chunk_desc through the
// skiplist chunk domain.
void desc_backing_free(DescBacking *backing) {
    if (LIBC_UNLIKELY(backing == nullptr))
        __builtin_trap();

    PerBackingState &p = g_backing_state;

    RecoveredSlot rec = recover_slot_from_va(
        backing, kBackingChunkBytes, kBackingSlotSize,
        kBackingSlotsPerChunk, kChunksPerPoolBucket, p.chunk_table);

    validate_slot_canaries_or_trap(
        backing->node_canary, rec.cd,
        static_cast<uint16_t>(
            partition_ns::PartitionClass::VaTrackerDescBacking),
        static_cast<uint8_t>(rec.chunk_id),
        static_cast<uint8_t>(rec.slot_idx), partition_secret());

    __builtin_memset(static_cast<void *>(backing), 0, sizeof(DescBacking));
    release_slot_in_va_chunk(rec.cd, rec.slot_idx, p.chunk_table);
}

//===----------------------------------------------------------------------===//
// Bootstrap
//===----------------------------------------------------------------------===//

void backing_init() {
    // ACQ_REL on the loser's exchange synchronises with the winner's
    // exchange so the loser observes init_registration's writes to the
    // domain by the time backing_init returns and the loser starts using
    // the domain. RELAXED here is wrong: loser would return having seen
    // init_done == 1 but stale domain internals.
    if (g_backing_init_done.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;
    g_va_tracker_backing_domain.init_registration();
    // chunk_table[] stays BSS-zero; first `backing_alloc` lazily commits the
    // first chunk via `commit_new_va_chunk_for`. Do not pre-commit here —
    // keeps the cold-path budget on first allocation, not init.
}

//===----------------------------------------------------------------------===//
// Fork
//===----------------------------------------------------------------------===//

// Three phases:
//   1. Drop the survivor's pins so the post-fork snapshot doesn't capture
//      a held era.
//   2. Walk populated chunk_table entries, refresh per-chunk canary against
//      the rotated partition_secret, then sweep the occupancy bitmap to
//      refresh per-slot canaries. Without this the next FreeFn on a
//      survivor slot would canary-mismatch and trap.
//   3. Stranded-slot reclaim. The skiplist subsystem's fork hook excludes
//      bucket kPoolBucketDescBacking from its filter — owned here. The
//      reachability bitmap is populated during phase 2; the visitor
//      decommits + releases any DescBacking chunk_desc claimed in the
//      shared pool but unreachable from our chunk_table.

namespace {

struct BackingForkReclaimCtx {
    static constexpr uint32_t kBitsetWords =
        (kTotalVaChunkDescPoolSize + 63) / 64;
    uint64_t reachable_bits[kBitsetWords];
};

LIBC_INLINE void mark_backing_chunk_reachable(BackingForkReclaimCtx &ctx,
                                                VaChunkDesc *cd) {
    if (cd == nullptr)
        return;
    uint32_t idx = va_chunk_pool_index_of(cd);
    ctx.reachable_bits[idx / 64] |= uint64_t{1} << (idx % 64);
}

// Phase-3 visitor. The decommit pairs with the partition_ns::commit_chunk
// that ran at chunk birth — without it the partition's commit accounting
// drifts every fork.
void backing_fork_reclaim_visit(VaChunkDesc *cd, void *ctx_p) {
    auto *ctx = static_cast<BackingForkReclaimCtx *>(ctx_p);
    if (cd->bucket_id != kPoolBucketDescBacking)
        return;
    uint32_t idx = va_chunk_pool_index_of(cd);
    if (ctx->reachable_bits[idx / 64] & (uint64_t{1} << (idx % 64)))
        return;

    if (cd->partition != nullptr && cd->chunk_base != nullptr) {
        partition_ns::decommit_chunk(cd->partition, cd->chunk_base,
                                     cd->chunk_bytes);
    }
    mark_dead(cd->live_state);
    va_chunk_desc_pool_release(cd);
}

// refresh_slot_canaries_in_chunk dispatches here per live slot with the
// pre-computed canary. DescBacking carries no other per-slot post-fork
// repair.
void backing_per_slot_canary_refresh(void *slot_va, uint64_t fresh_canary) {
    auto *b = static_cast<DescBacking *>(slot_va);
    b->node_canary = fresh_canary;
}

} // namespace

void backing_fork_reinit() {
    g_va_tracker_backing_domain.clear_all();

    BackingForkReclaimCtx reclaim_ctx{};
    constexpr uint16_t kBackingClsId = static_cast<uint16_t>(
        partition_ns::PartitionClass::VaTrackerDescBacking);
    for (uint32_t cid = 0; cid < kChunksPerPoolBucket; ++cid) {
        VaChunkDesc *cd = g_backing_state.chunk_table[cid].load(
            cpp::MemoryOrder::ACQUIRE);
        if (cd == nullptr)
            continue;
        mark_backing_chunk_reachable(reclaim_ctx, cd);
        cd->chunk_canary = compute_va_chunk_canary(
            partition_secret(), kBackingClsId, static_cast<uint8_t>(cid));
        refresh_slot_canaries_in_chunk(
            cd, kBackingClsId, static_cast<uint8_t>(cid),
            kBackingSlotsPerChunk, partition_secret(),
            &backing_per_slot_canary_refresh);
    }

    for_each_claimed_va_chunk_desc(&backing_fork_reclaim_visit, &reclaim_ctx);
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
