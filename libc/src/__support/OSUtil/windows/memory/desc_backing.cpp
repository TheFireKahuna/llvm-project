//===- desc_backing.cpp - Backing allocator + Stage 3 FreeFn --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Backing-pool allocator, BatchLinkCodec, and Crystalline-W FreeFn for
// the Layer 1 `DescBacking` body. Every path through this translation
// unit performs pure userspace work — slot canary validation, bitmap
// clears, partition pool release. No kernel-primitive call appears
// anywhere in this file; the CI grep gate enforces it. The
// synchronous kernel-state teardown (handle close, placeholder free,
// view unmap) lives next door in `va_tracker_transaction.cpp`, where
// the transaction's commit owns the kernel state per the
// va_tracker's mutator-owns-kernel-state discipline (Nikolaev and
// Ravindran, PLDI 2024, §1: Crystalline-W is asynchronous and has no
// synchronous grace primitive).
//
// Allocation routes through the shared `va_chunk_acquire_slot`
// scaffold in `va_tracker_chunk.h`; the per-slot publish (generation
// seed, cached coords, Live state, canary stamp, Crystalline init)
// runs as the spec's init callback. FreeFn and fork canary refresh
// use the shared `recover_slot_from_va` / `refresh_slot_canaries_in_chunk`
// helpers so the slot-recovery shape is expressed once for every
// va_tracker pool consumer.
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

//===----------------------------------------------------------------------===//
// Crystalline-W backing domain definition
//===----------------------------------------------------------------------===//

// Self-installs from `backing_init()`, which is invoked from
// `va_tracker_init_fn` after the skiplist init.
::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    DescBacking, &desc_backing_free, kBackingRetireFreq>
    g_va_tracker_backing_domain;

//===----------------------------------------------------------------------===//
// Per-class chunk-table state
//===----------------------------------------------------------------------===//

// Per-class chunk table for the VaTrackerDescBacking partition. Identical
// shape to `PerRegionDescState` and `PerArenaState`; the chunk-bitmap scan,
// Crystalline-pinned chunk lookup, and rotating hint all ride the shared
// `va_chunk_acquire_slot` scaffold in `va_tracker_chunk.h`.
// Process-lifetime; CoW-inherited across fork; the fork-reinit hook
// refreshes canaries and reclaims stranded slots without touching the
// table layout.

namespace {

constexpr uint32_t kBackingSlotSize = sizeof(DescBacking);          // 64
constexpr uint32_t kBackingSlotsPerChunk = kMaxSlotsPerChunk;       // 256
// 64 KiB chunk_bytes for pagemap alignment. With 256 slots * 64 B =
// 16 KiB of payload, the trailing 48 KiB is committed-but-unused —
// the same bounded commit-charge cost the RegionDesc partition pays.
constexpr uint32_t kBackingChunkBytes = 64u * 1024u;

static_assert(kBackingChunkBytes == 64u * 1024u,
              "DescBacking chunk must be exactly 64 KiB");
static_assert(kBackingSlotSize * kBackingSlotsPerChunk <=
                  kBackingChunkBytes,
              "DescBacking slot range overflows chunk");
static_assert(kBackingSlotSize >= sizeof(DescBacking),
              "DescBacking slot must hold a full DescBacking object");

struct alignas(64) PerBackingState {
    cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerPoolBucket]{};
    alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
};

PerBackingState g_backing_state;

// One-shot init flag. `init_registration()` is one-shot by
// Crystalline-W contract — callers must not invoke it twice on the
// same domain.
cpp::Atomic<uint32_t> g_backing_init_done{0};

LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Derive a non-zero starting generation for a fresh allocation by
// mixing `partition_secret` with the slot coordinates. Keeps a fresh
// slot's generation unguessable across fork (rotated through
// `partition_secret`) and ensures the encoded BackingRef never
// collides with `kBackingRefNull` at the (0, 0) slot.
[[nodiscard]] LIBC_INLINE uint32_t
seed_generation_for_slot(uint16_t chunk_id, uint16_t slot_idx) {
    uint64_t s = partition_secret();
    uint32_t mixed = static_cast<uint32_t>((s ^ (s >> 32)) +
                                            (uint32_t{chunk_id} << 16) +
                                            uint32_t{slot_idx});
    return mixed == 0 ? 1u : mixed;
}

// Per-slot initializer invoked by `va_chunk_acquire_slot` after the
// slot has been bitmap-claimed, memset to zero, and had its chunk and
// slot canaries validated against the pool's class id. Seeds the
// generation, stamps the cached coords, stamps state Live, computes
// the node canary, and stamps the Crystalline birth_era.
void backing_init_slot(void *slot, VaChunkDesc * /*cd*/,
                       uint32_t chunk_id, uint32_t slot_idx,
                       void * /*ctx*/) {
    auto *b = static_cast<DescBacking *>(slot);
    // Seed generation to a non-zero value so the encoded ref never
    // collides with `kBackingRefNull`.
    b->generation.store(
        seed_generation_for_slot(static_cast<uint16_t>(chunk_id),
                                  static_cast<uint16_t>(slot_idx)),
        cpp::MemoryOrder::RELAXED);
    // Cache (chunk_id, slot_idx) so encoders never need a
    // pagemap_load_descriptor roundtrip. Plain (non-atomic) writes:
    // the descriptor that captures this slot's `BackingRef` is
    // published via Swap CAS on `pred->next[0]`, which provides the
    // cross-thread happens-before for any reader that arrives through
    // the published desc. Both coordinates fit in u8 because
    // `kChunksPerPoolBucket` and `kSlotsPerChunk` are both 256.
    b->cached_chunk_id = static_cast<uint8_t>(chunk_id);
    b->cached_slot_idx = static_cast<uint8_t>(slot_idx);
    // Freshly allocated backings are always Live. The caller (a
    // Transaction visitor) is the sole owner until it publishes a
    // desc that references this backing through Swap; the eventual
    // Live -> Killed transition is owned by either the post-Swap
    // survivor walk (when no LIVE desc references the backing) or
    // the Transaction's rollback path (STEP 2 / STEP 3 failure).
    // Kernel-state atomics remain nullptr until
    // `backing_set_kernel_state` runs.
    b->state.store(kBackingStateLive, cpp::MemoryOrder::RELAXED);
    b->node_canary = compute_va_node_canary(
        partition_secret(),
        static_cast<uint16_t>(
            partition_ns::PartitionClass::VaTrackerDescBacking),
        static_cast<uint8_t>(chunk_id), static_cast<uint8_t>(slot_idx));
    // Stamp the Crystalline-W birth_era and zero the batch_link so
    // the slot is ready to ride future retire batches.
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
    // Read the cached (chunk_id, slot_idx) directly from the slot.
    // `backing_alloc` stamps both coordinates once at slot
    // initialisation and they never change; the descriptor publish
    // (Swap CAS on `pred->next[0]`) provides the cross-thread
    // happens-before for any reader that arrives via a published
    // desc, and this codec is invoked by Crystalline retirement,
    // which is downstream of that publish.
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

// Two-phase acquire: hand the shared va_chunk allocator a spec
// describing the partition class and slot geometry, then loop — pull
// a slot from any live chunk if one is available, otherwise commit a
// fresh chunk and retry. A null return from `commit_new_va_chunk_for`
// is the only terminal failure. The chunk pin, the rotating-hint
// scan, and the per-slot publish (generation seed, cached coords,
// Live state, node canary, Crystalline init) all ride the shared
// `va_chunk_acquire_slot` scaffold; `backing_init_slot` carries the
// pool-specific publish.
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
    // Shape first because it's plain (non-atomic) and read by
    // `backing_kill_and_retire` to dispatch between the
    // unmap-then-free and free-only teardowns. The kernel-state
    // RELEASE stores below feed the readers' ACQUIRE load, so by the
    // time any reader observes a non-null `placeholder_base` the
    // shape store is also visible.
    backing->shape = shape;
    // Handles first, base last. Stage 2's null-store follows the
    // same order so a "load handle, close handle" sequence never
    // observes a closed handle paired with a non-null base.
    backing->section_handle.store(section_handle, cpp::MemoryOrder::RELEASE);
    backing->file_handle.store(file_handle, cpp::MemoryOrder::RELEASE);
    // `placeholder_pages` is plain (non-atomic): set once per
    // backing lifetime, and the descriptor publish (Swap CAS on
    // `pred->next[0]`) provides the cross-thread happens-before.
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

    // STAGE 2 — canaries. Chunk canary first (catches chunk_id
    // transposition / partition mismatch); slot canary second
    // (catches slot tampering).
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

    // STAGE 3 — generation. ACQUIRE pairs with the RELEASE store
    // at alloc time; a recycled-slot observation carries a different
    // generation and traps here.
    uint32_t live_gen = b->generation.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(live_gen != generation))
        __builtin_trap();

    return b;
}

//===----------------------------------------------------------------------===//
// Cross-domain pin defence — anchor pins on the backing domain
//===----------------------------------------------------------------------===//

// Anchor thunk for `protect()`. Returns nullptr because the call's
// value is irrelevant; only the era-stability convergence inside
// `protect()` matters. Pure (no captures, no side effects beyond the
// read of its argument); safe to invoke from a foreign helper thread
// per CrystallineDomain's helping-protocol thunk contract.
namespace {
inline DescBacking *backing_anchor_thunk(void *) { return nullptr; }
struct BackingAnchorCtx {};
} // namespace

// Engine path: pins `BackingPinSlot::kEngineAnchor` for the rest of
// the run_envelope retry loop. The slot is never rotated by sibling
// va_tracker calls inside the envelope, so one call covers every
// attempt.
void anchor_backing_engine_pin() {
    BackingAnchorCtx ctx{};
    (void)g_va_tracker_backing_domain.protect<&backing_anchor_thunk>(
        ctx, BackingPinSlot::kEngineAnchor, /*parent=*/nullptr);
}

// Reader path: pins `BackingPinSlot::kReaderPin` for the calling
// thread. Crystalline-W `protect()` is non-cumulative, so the pinned
// era stays in effect until the next `protect()` on the same slot
// (typically the next call to this function on this thread), at
// `clear_all()`, or at thread exit. Reader sites that need
// inter-field staleness defence within the pinned scope wrap the raw
// deref's return in `BackingView`.
void anchor_backing_reader_pin() {
    BackingAnchorCtx ctx{};
    (void)g_va_tracker_backing_domain.protect<&backing_anchor_thunk>(
        ctx, BackingPinSlot::kReaderPin, /*parent=*/nullptr);
}

//===----------------------------------------------------------------------===//
// Stage 3 metadata FreeFn
//===----------------------------------------------------------------------===//

// Body cleanup only — no `nt_pal::*` and no `NtClose` may appear in
// this function. Every byte of kernel resource was torn down at
// Stage 2 (`backing_kill_and_retire` in
// `va_tracker_transaction.cpp`) before Crystalline grace expired
// and this FreeFn fired. The `partition_secret()` reads are pure
// math; they do not reach into NT. A CI grep gate enforces the
// invariant on this translation unit.
//
// Triple-validates (canary first, defending against heap-spray),
// zero-fills the slot, and returns it via the chunk-state-machine
// drain. The drain may transition the chunk to Draining if this was
// the last live slot, in which case the chunk descriptor itself
// retires through the skiplist chunk domain.
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
    if (g_backing_init_done.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;
    g_va_tracker_backing_domain.init_registration();
    // `chunk_table[]` is BSS-zero-initialised; the first
    // `backing_alloc` lazily commits the first chunk via
    // `commit_new_va_chunk_for`.
}

//===----------------------------------------------------------------------===//
// Fork
//===----------------------------------------------------------------------===//

// Mirrors `interval_skiplist_fork_reinit` in three phases:
//   1. Drop the surviving thread's pins on the backing domain so the
//      post-fork snapshot does not capture a held era.
//   2. Walk every populated chunk_table entry, refresh per-chunk
//      canary against the rotated `partition_secret`, then sweep the
//      occupancy bitmap to refresh per-slot canaries on every live
//      backing. Without this pass the next FreeFn on a survivor slot
//      would see a canary mismatch and trap.
//   3. Stranded-slot reclaim. The skiplist subsystem's fork hook
//      excludes bucket 6 (DescBacking) from its reclaim filter — that
//      bucket is owned here. The walk reuses the shared
//      `for_each_claimed_va_chunk_desc` API; the marker bitmap is
//      built locally during phase 2 to identify DescBacking chunks
//      reachable from `g_backing_state.chunk_table[]`, and the
//      callback decommits and returns the slots of any DescBacking
//      chunk_desc that's claimed in the pool but unreachable from
//      our chunk_table.

namespace {

// Stranded-chunk reclaim context. Mirrors `ForkReclaimCtx` in
// `interval_skiplist.cpp`.
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

// Reclaim-visitor for phase 3. Skip chunks that aren't ours, skip
// chunks reachable from `chunk_table[]`, decommit + release the rest.
// The chunk's pages were committed under `partition_ns::commit_chunk`
// at chunk birth and are balanced here by the matching
// `partition_ns::decommit_chunk` so the partition's commit accounting
// stays balanced.
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

// Per-slot fork canary refresh callback. The shared
// `refresh_slot_canaries_in_chunk` walks the occupancy bitmap, computes
// the fresh canary for each live slot, and dispatches here with the
// slot VA and pre-computed value. DescBacking carries no additional
// per-slot post-fork repair beyond the canary write.
void backing_per_slot_canary_refresh(void *slot_va, uint64_t fresh_canary) {
    auto *b = static_cast<DescBacking *>(slot_va);
    b->node_canary = fresh_canary;
}

} // namespace

void backing_fork_reinit() {
    // Phase 1: drop pins on the surviving thread.
    g_va_tracker_backing_domain.clear_all();

    // Phase 2: refresh canaries against the rotated `partition_secret`
    // and mark every reachable chunk for the stranded-chunk reclaim.
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

    // Phase 3: stranded-chunk reclaim. Walks the shared chunk-desc
    // pool and reclaims any DescBacking chunk_desc whose chunk_table
    // entry was lost (parent's Crystalline cell held the retire batch
    // and that thread is gone in the child).
    for_each_claimed_va_chunk_desc(&backing_fork_reclaim_visit, &reclaim_ctx);
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
