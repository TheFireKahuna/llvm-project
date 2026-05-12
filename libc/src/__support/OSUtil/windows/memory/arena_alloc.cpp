//===- arena_alloc.cpp - va_tracker Arena allocator ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the va_tracker Arena allocator: the Crystalline-W domain
// instance, the VaTrackerArena partition's per-class chunk_table, the
// per-slot initializer that runs after va_chunk_acquire_slot, and the
// FreeFn that triple-validates a retired Arena and returns its slot to
// the shared VaChunkDesc pool.
//
// The Arena is the per-(ART-leaf, CPU) anchor of the interval skiplist
// (Kim, Kwon & Kang, SOSP 2025): it owns the sentinel head node, the
// hint cell used by paper §"Alloc" as a walk-start, and the lo/hi range
// the skiplist covers. Skiplist nodes themselves come from the separate
// height-bucketed allocator in interval_skiplist.cpp.
//
// Crystalline-W reclamation (Nikolaev & Ravindran, PLDI 2024) is
// asynchronous by design and provides no synchronous-grace primitive;
// arena_free runs as a FreeFn invoked by foreign threads' pin drains
// long after retire(). The allocator scan loop is the shared
// va_chunk_acquire_slot scaffold from va_tracker_chunk.h; this TU
// supplies only the Arena-specific init function and the FreeFn.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/arena_alloc.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/skiplist_link_traits.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

using ::LIBC_NAMESPACE::linkage::Link;
using ::LIBC_NAMESPACE::linkage::link_cas_state;
namespace partition_ns = alloc::partition;

//===----------------------------------------------------------------------===//
//  Arena partition geometry
//===----------------------------------------------------------------------===//
//
// 64 KiB chunk_bytes pins the chunk to a single pagemap stamping unit
// so a typed pagemap_load_descriptor recovers cd from any address
// inside an Arena slot in O(1). floor(65536 / 320) = 204 slots per
// chunk; the 256 leftover bytes (~0.4%) are the cost of the
// pagemap-alignment constraint. 320 B per slot is the smallest size
// that contains a full Arena (including its inline sentinel head and
// the tail levels of head_tail_links_).

constexpr uint32_t kArenaSlotSize       = 320;
constexpr uint32_t kArenaChunkBytes     = 64u * 1024u;
constexpr uint32_t kArenaSlotsPerChunk  = kArenaChunkBytes / kArenaSlotSize; // 204

static_assert(kArenaSlotsPerChunk <= 256,
              "kArenaSlotsPerChunk must fit 8-bit slot_idx");
static_assert((kArenaChunkBytes & 0xFFFF) == 0,
              "Arena chunk must be 64 KiB-aligned for pagemap registration");
static_assert(kArenaChunkBytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "Arena chunk_bytes must be a multiple of pagemap stamping "
              "granularity");
static_assert(kArenaSlotSize >= sizeof(Arena),
              "Arena slot must hold a full Arena object");
static_assert(static_cast<uint64_t>(kArenaSlotSize) *
                  static_cast<uint64_t>(kArenaSlotsPerChunk) <=
                  kArenaChunkBytes,
              "Arena slot range overflows chunk");

namespace {

// Per-class allocator state for the Arena partition. The chunk_table
// is indexed by chunk_id; next_chunk_id_hint is the rotating scan
// start used by va_chunk_acquire_slot to amortise scan cost across
// concurrent allocators. Cache-line aligned on both fields to avoid
// false sharing between the allocator hot path and the FreeFn.
struct alignas(64) PerArenaState {
    cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerBucket]{};
    alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
};

PerArenaState g_arena_state;

// Diagnostic counter — RELAXED throughout, never a synchronisation
// edge. Read by arena_live_count() for stats_snapshot only.
cpp::Atomic<uint32_t> g_live_arenas{0};

LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Init context threaded through va_chunk_acquire_slot to arena_init_slot.
// The shared scaffold is generic over the slot type, so per-allocator
// constants ride in this struct rather than being curried in.
struct ArenaInitCtx {
    uintptr_t arena_lo;
    uint32_t cpu_index;
};

// Per-slot initializer invoked by va_chunk_acquire_slot after the slot
// has been bitmap-claimed, memset to zero, and had its chunk and slot
// canaries validated. Populates the Arena's identity fields, builds
// the sentinel head, and stamps the Crystalline birth_era.
void arena_init_slot(void *slot, VaChunkDesc * /*cd*/,
                     uint32_t chunk_id, uint32_t slot_idx, void *ctx_p);

} // namespace

//===----------------------------------------------------------------------===//
//  Crystalline-W domain instance
//===----------------------------------------------------------------------===//
//
// Per Nikolaev & Ravindran PLDI 2024, the FreeFn (arena_free below)
// runs once an Arena has cleared every reservation slot — i.e. after
// every reader that may have observed the pointer at retire time has
// either decremented its reservation or itself drained the retire
// batch. The FreeFn drains reachable nodes, validates the canary,
// memsets the slot, and returns it to the shared pool via
// release_slot_in_va_chunk.

::LIBC_NAMESPACE::concurrent::CrystallineDomain<Arena, &arena_free,
                                                kSkiplistRetireFreq>
    g_va_tracker_arena_domain;

void arena_init_registration() {
    g_va_tracker_arena_domain.init_registration();
}

uint32_t arena_live_count() {
    return g_live_arenas.load(cpp::MemoryOrder::RELAXED);
}

} // namespace va_tracker
} // namespace windows

//===----------------------------------------------------------------------===//
//  BatchLinkCodec specialization for Arena
//===----------------------------------------------------------------------===//
//
// Crystalline-W retire batches encode each retired pointer as a 32-bit
// codec word so a batch fits in a single cache line. The Arena
// partition publishes typed pagemap entries (VaTrackerVaChunk
// consumer), so a single pagemap_load_descriptor recovers the
// VaChunkDesc from any address inside an Arena slot — encoding is
// pagemap lookup plus subtract-and-divide, decoding is chunk_table
// lookup plus multiply-and-add. The +1 / -1 keeps code 0 reserved as
// the batch's empty sentinel.

namespace concurrent {

uint32_t BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::Arena>::encode(
    CrystallineNode *n) noexcept {
    using Node = ::LIBC_NAMESPACE::windows::va_tracker::Arena;
    auto *a = static_cast<Node *>(n);
    auto *cd = ::LIBC_NAMESPACE::windows::alloc::pagemap_load_descriptor<
        ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer::VaTrackerVaChunk>(a);
    uint32_t slot_idx = static_cast<uint32_t>(
        (reinterpret_cast<uintptr_t>(a) -
         reinterpret_cast<uintptr_t>(cd->chunk_base)) /
        ::LIBC_NAMESPACE::windows::va_tracker::kArenaSlotSize);
    return 1u + ((static_cast<uint32_t>(cd->chunk_id) << 8) | slot_idx);
}

CrystallineNode *
BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::Arena>::decode(
    uint32_t code) noexcept {
    using Node = ::LIBC_NAMESPACE::windows::va_tracker::Arena;
    uint32_t v = code - 1u;
    uint32_t chunk_id = (v >> 8) & 0xFFu;
    uint32_t slot_idx = v & 0xFFu;
    // ACQUIRE pairs with the RELEASE-CAS that publishes a fresh chunk
    // descriptor in commit_new_va_chunk_for; the decoder must observe
    // a fully-initialised cd or none at all.
    auto *cd = ::LIBC_NAMESPACE::windows::va_tracker::g_arena_state
                   .chunk_table[chunk_id]
                   .load(cpp::MemoryOrder::ACQUIRE);
    auto *base = static_cast<char *>(cd->chunk_base);
    return reinterpret_cast<Node *>(
        base +
        slot_idx * ::LIBC_NAMESPACE::windows::va_tracker::kArenaSlotSize);
}

} // namespace concurrent

namespace windows {
namespace va_tracker {

namespace {

void arena_init_slot(void *slot, VaChunkDesc * /*cd*/,
                     uint32_t chunk_id, uint32_t slot_idx, void *ctx_p) {
    auto *ctx = static_cast<ArenaInitCtx *>(ctx_p);
    auto *arena = static_cast<Arena *>(slot);

    arena->arena_lo = ctx->arena_lo;
    // 4 GiB stride per ART leaf — matches the current ART key encoding
    // that uses the upper 32 bits of VA as the leaf address.
    arena->arena_hi = ctx->arena_lo + (uintptr_t{1} << 32);
    arena->cpu_index = ctx->cpu_index;
    arena->arena_canary = compute_va_node_canary(
        partition_secret(),
        static_cast<uint16_t>(partition_ns::PartitionClass::VaTrackerArena),
        static_cast<uint8_t>(chunk_id), static_cast<uint8_t>(slot_idx));

    arena->head.lo = arena->arena_lo;
    arena->head.hi = arena->arena_hi;
    arena->head.height = kMaxHeight;
    // 0xFF in bucket / chunk_id / slot_idx marks the sentinel head;
    // the head lives inline in the Arena and is not allocated from a
    // bucket, so the substrate's "decode from chunk_id/slot_idx" path
    // must reject it.
    arena->head.bucket = 0xFF;
    arena->head.chunk_id = 0xFF;
    arena->head.slot_idx = 0xFF;
    arena->head.owning_arena = arena;
    arena->head.node_canary = arena->arena_canary;
    arena->head.value.store(nullptr, cpp::MemoryOrder::RELAXED);
    // All level links start LIVE with a null encoding so the first
    // is_insert sees an empty chain.
    arena->head.next[0].store(
        Link::pack(static_cast<uint8_t>(SkiplistNodeState::LIVE),
                   /*tag=*/0, kLinkNullEncoding),
        cpp::MemoryOrder::RELAXED);
    for (uint32_t i = 0; i < kMaxHeight - 1; ++i) {
        arena->head_tail_links_[i].store(
            Link::pack(static_cast<uint8_t>(SkiplistNodeState::LIVE),
                       /*tag=*/0, kLinkNullEncoding),
            cpp::MemoryOrder::RELAXED);
    }

    g_va_tracker_arena_domain.init_node(arena);
    g_live_arenas.fetch_add(1, cpp::MemoryOrder::RELAXED);
}

} // namespace

//===----------------------------------------------------------------------===//
//  arena_alloc — allocation hot path
//===----------------------------------------------------------------------===//
//
// Two-phase per the shared chunk scaffold: Phase 1 scans the existing
// chunk_table via va_chunk_acquire_slot, Phase 2 commits a fresh chunk
// through commit_new_va_chunk_for on Phase-1 failure and retries. The
// retry loop is bounded by kChunksPerBucket × kArenaSlotsPerChunk
// (substrate cap); exhausting it means the per-class allocator is full
// and the caller observes nullptr.

Arena *arena_alloc(uintptr_t arena_lo, uint32_t cpu_index) {
    PerArenaState &a = g_arena_state;
    ArenaInitCtx ctx{arena_lo, cpu_index};

    VaChunkAcquireSpec spec{
        /*cls=*/partition_ns::PartitionClass::VaTrackerArena,
        /*chunk_table=*/a.chunk_table,
        /*next_chunk_id_hint=*/&a.next_chunk_id_hint,
        /*chunk_count=*/kChunksPerBucket,
        /*slots_per_chunk=*/kArenaSlotsPerChunk,
        /*consumer_bucket_id=*/kPoolBucketArena,
        /*init=*/&arena_init_slot,
        /*init_ctx=*/&ctx,
    };

    for (;;) {
        void *slot = va_chunk_acquire_slot(spec);
        if (slot != nullptr)
            return static_cast<Arena *>(slot);

        // Phase 1 returned no free slot in any Live chunk. Commit a
        // fresh chunk and retry; commit_new_va_chunk_for tolerates
        // races (a peer may install a different chunk at the same
        // chunk_id, in which case our reservation is rolled back).
        VaChunkDesc *cd = commit_new_va_chunk_for(
            partition_ns::PartitionClass::VaTrackerArena, a.chunk_table,
            &a.next_chunk_id_hint, /*bucket_id=*/kPoolBucketArena,
            kArenaSlotSize, kArenaSlotsPerChunk, kArenaChunkBytes);
        if (cd == nullptr)
            return nullptr;
    }
}

void arena_retire(Arena *arena) {
    if (LIBC_UNLIKELY(arena == nullptr))
        return;
    g_va_tracker_arena_domain.retire(arena);
}

//===----------------------------------------------------------------------===//
//  arena_free — Crystalline-W FreeFn
//===----------------------------------------------------------------------===//

namespace {

// Best-effort drain of the Arena's level-0 node chain.
//
// By the time arena_free dispatches here, the Arena has passed
// Crystalline grace and no concurrent op can reach its nodes. The
// drain walks level 0 and, for each reachable node, validates the
// per-slot canary, CASes LIVE -> INVALIDATED (publishing
// LINK_ALERT_FIRED_BIT through SkiplistLinkTraits::fires_alert in the
// same atomic), and retires the node through the skiplist-node
// domain. The CAS cannot legitimately fail in this window — a failure
// means another writer reached a reachable node behind grace, which
// breaks the substrate's reachability invariant.
void drain_arena_nodes(Arena *arena) {
    uint16_t enc =
        arena->head_next(0).load(cpp::MemoryOrder::ACQUIRE).next();
    while (enc != kLinkNullEncoding) {
        SkiplistNodeBase *node = resolve_link_target(enc);
        if (node == nullptr)
            break;

        // Defence-in-depth: validate node_canary BEFORE dereferencing
        // any field that drives retire. resolve_link_target has
        // already cross-checked chunk_id/slot_idx field consistency;
        // a stale node_canary means the slot was tampered with after
        // its post-init canary was stamped.
        BucketGeometry geom = bucket_geometry(node->bucket);
        uint64_t expected_canary = compute_va_node_canary(
            partition_secret(), static_cast<uint16_t>(geom.cls),
            node->chunk_id, node->slot_idx);
        if (LIBC_UNLIKELY(node->node_canary != expected_canary))
            __builtin_trap();
        Link next = node->next[0].load(cpp::MemoryOrder::ACQUIRE);
        enc = next.next();
        node->owning_arena = nullptr;

        // Typed CAS — the substrate ALERT_FIRED bit publishes in the
        // same atomic as the LIVE -> INVALIDATED transition per
        // SkiplistLinkTraits::fires_alert. Past grace, every
        // reachable node is LIVE; a CAS-fail here is the invariant
        // break detector.
        const bool cas_ok =
            link_cas_state<SkiplistNodeState::LIVE,
                           SkiplistNodeState::INVALIDATED,
                           SkiplistLinkTraits>(node->next[0]);
        if (LIBC_UNLIKELY(!cas_ok))
            __builtin_trap();
        g_va_tracker_skiplist_domain.retire(node);
    }
    arena->hint.store(nullptr, cpp::MemoryOrder::RELAXED);
    // Reset every head link to LIVE/null so the slot is reusable
    // immediately on release_slot_in_va_chunk.
    for (uint32_t lvl = 0; lvl < kMaxHeight; ++lvl) {
        arena->head_next(lvl).store(
            Link::pack(static_cast<uint8_t>(SkiplistNodeState::LIVE),
                       /*tag=*/0, kLinkNullEncoding),
            cpp::MemoryOrder::RELAXED);
    }
}

} // namespace

void arena_free(Arena *arena) {
    // Recover (cd, chunk_id, slot_idx) from the Arena VA and
    // triple-validate canaries BEFORE touching any chunk-descriptor
    // field. The path from "external pointer" to "trusted descriptor"
    // must catch every corruption it can before it reaches a real pool
    // entry. The shared helper expresses this once for every va_tracker
    // pool consumer.
    PerArenaState &a = g_arena_state;
    RecoveredSlot rec = recover_slot_from_va(
        arena, kArenaChunkBytes, kArenaSlotSize, kArenaSlotsPerChunk,
        kChunksPerBucket, a.chunk_table);

    // Per-slot canary check uses Arena::arena_canary as the node canary
    // (it is the per-slot canary under the va_tracker derivation).
    validate_slot_canaries_or_trap(
        arena->arena_canary, rec.cd,
        static_cast<uint16_t>(partition_ns::PartitionClass::VaTrackerArena),
        static_cast<uint8_t>(rec.chunk_id),
        static_cast<uint8_t>(rec.slot_idx), partition_secret());

    drain_arena_nodes(arena);
    g_live_arenas.fetch_sub(1, cpp::MemoryOrder::RELAXED);
    __builtin_memset(static_cast<void *>(arena), 0, sizeof(Arena));
    release_slot_in_va_chunk(rec.cd, rec.slot_idx, a.chunk_table);
}

//===----------------------------------------------------------------------===//
//  Fork-reinit
//===----------------------------------------------------------------------===//

namespace {

// Per-slot fork canary refresh callback. The shared
// `refresh_slot_canaries_in_chunk` walks the occupancy bitmap and
// dispatches here with the slot VA and pre-computed canary. Arena
// additionally writes the same canary into its inline sentinel head
// (which shares the per-slot derivation by construction) and scrubs
// stale LOCKED on the head's level-0 link as belt-and-braces; an
// Arena head should never carry LOCKED in steady state, but a
// corrupted pre-fork state would otherwise park a future parker
// indefinitely.
void arena_per_slot_canary_refresh(void *slot_va, uint64_t fresh_canary) {
    auto *arena = static_cast<Arena *>(slot_va);
    arena->arena_canary = fresh_canary;
    arena->head.node_canary = fresh_canary;
    (void)link_cas_state<SkiplistNodeState::LOCKED, SkiplistNodeState::LIVE,
                          SkiplistLinkTraits>(arena->head.next[0]);
}

} // namespace

void arena_fork_reinit_phase(ArenaForkChunkVisitor visit, void *ctx) {
    g_va_tracker_arena_domain.clear_all();
    constexpr uint16_t kArenaClsId = static_cast<uint16_t>(
        partition_ns::PartitionClass::VaTrackerArena);
    for (uint32_t cid = 0; cid < kChunksPerBucket; ++cid) {
        VaChunkDesc *cd =
            g_arena_state.chunk_table[cid].load(cpp::MemoryOrder::ACQUIRE);
        if (cd == nullptr)
            continue;
        if (visit != nullptr)
            visit(cd, ctx);
        cd->chunk_canary = compute_va_chunk_canary(
            partition_secret(), kArenaClsId, static_cast<uint8_t>(cid));
        refresh_slot_canaries_in_chunk(
            cd, kArenaClsId, static_cast<uint8_t>(cid),
            kArenaSlotsPerChunk, partition_secret(),
            &arena_per_slot_canary_refresh);
    }
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
