//===- arena_alloc.cpp - va_tracker Arena allocator ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-(ART-leaf, CPU) anchor of the interval skiplist (Kim, Kwon & Kang,
// SOSP 2025): the Arena owns the sentinel head, the paper "Alloc"
// walk-start hint, and the lo/hi range the skiplist covers. Skiplist
// nodes are allocated separately in interval_skiplist.cpp.
//
// Reclamation is Crystalline-W (Nikolaev & Ravindran, PLDI 2024) — no
// synchronous grace; arena_free runs as a FreeFn dispatched by foreign
// pin drains long after retire().
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
// 64 KiB chunk_bytes is one pagemap stamping unit, so pagemap_load_descriptor
// recovers cd in O(1) from any VA inside an Arena slot. 320 B/slot is the
// smallest size that holds a full Arena including its inline sentinel head
// and head_tail_links_ tail.

constexpr uint32_t kArenaSlotSize       = 320;
constexpr uint32_t kArenaChunkBytes     = 64u * 1024u;
constexpr uint32_t kArenaSlotsPerChunk  = kArenaChunkBytes / kArenaSlotSize;

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

// alignas isolates next_chunk_id_hint (allocator hot path) from chunk_table
// (also touched by the FreeFn) to avoid false sharing.
struct alignas(64) PerArenaState {
    cpp::Atomic<VaChunkDesc *> chunk_table[kChunksPerBucket]{};
    alignas(64) cpp::Atomic<uint32_t> next_chunk_id_hint{0};
};

PerArenaState g_arena_state;

// Diagnostic counter only; not a synchronisation edge — every access is
// RELAXED. Read by arena_live_count for stats_snapshot.
cpp::Atomic<uint32_t> g_live_arenas{0};

LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Threaded through va_chunk_acquire_slot to arena_init_slot via the
// scaffold's void* init_ctx slot — the scaffold is slot-type-generic and
// cannot carry typed per-allocator init constants in its own signature.
struct ArenaInitCtx {
    uintptr_t arena_lo;
    uint32_t cpu_index;
};

void arena_init_slot(void *slot, VaChunkDesc * /*cd*/,
                     uint32_t chunk_id, uint32_t slot_idx, void *ctx_p);

} // namespace

//===----------------------------------------------------------------------===//
//  Crystalline-W domain instance
//===----------------------------------------------------------------------===//

// Arena domain has no protect()/anchor() call sites — only init_node /
// retire / clear_all. MaxIdx = 1 minimises the unused reservation array.
inline constexpr uint32_t kArenaDomainMaxIdx = 1;

::LIBC_NAMESPACE::concurrent::CrystallineDomain<Arena, &arena_free,
                                                kSkiplistRetireFreq,
                                                kArenaDomainMaxIdx>
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
// Retire batches pack pointers into 32-bit codes so a batch fits in one
// cache line. Encoding goes via pagemap_load_descriptor; decoding via
// chunk_table. Code 0 is the batch's empty sentinel — hence the +1/-1.

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
    // ACQUIRE pairs with the RELEASE-CAS in commit_new_va_chunk_for that
    // publishes a fresh descriptor.
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
    // 4 GiB stride matches the ART key encoding (upper 32 bits of VA = leaf).
    arena->arena_hi = ctx->arena_lo + (uintptr_t{1} << 32);
    arena->cpu_index = ctx->cpu_index;
    arena->arena_canary = compute_va_node_canary(
        partition_secret(),
        static_cast<uint16_t>(partition_ns::PartitionClass::VaTrackerArena),
        static_cast<uint8_t>(chunk_id), static_cast<uint8_t>(slot_idx));

    arena->head.lo = arena->arena_lo;
    arena->head.hi = arena->arena_hi;
    arena->head.height = kMaxHeight;
    // 0xFF in bucket / chunk_id / slot_idx marks the inline sentinel head so
    // the substrate's decode-from-chunk_id/slot_idx path rejects it (the head
    // is not bucket-allocated).
    arena->head.bucket = 0xFF;
    arena->head.chunk_id = 0xFF;
    arena->head.slot_idx = 0xFF;
    arena->head.owning_arena = arena;
    arena->head.node_canary = arena->arena_canary;
    arena->head.value.store(nullptr, cpp::MemoryOrder::RELAXED);
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

        // No free slot in any Live chunk. commit_new_va_chunk_for tolerates
        // a peer installing first at the same chunk_id (our reservation is
        // rolled back).
        VaChunkDesc *cd = commit_new_va_chunk_for(
            partition_ns::PartitionClass::VaTrackerArena, a.chunk_table,
            &a.next_chunk_id_hint, /*bucket_id=*/kPoolBucketArena,
            kArenaSlotSize, kArenaSlotsPerChunk, kArenaChunkBytes);
        if (cd == nullptr)
            return nullptr;
    }
}

void arena_retire(Arena *arena) {
    // Public surface — nullptr-tolerant even though the current sole caller
    // pre-checks; the domain's retire() would otherwise deref null.
    if (LIBC_UNLIKELY(arena == nullptr))
        return;
    g_va_tracker_arena_domain.retire(arena);
}

//===----------------------------------------------------------------------===//
//  arena_free — Crystalline-W FreeFn
//===----------------------------------------------------------------------===//

namespace {

// Past Crystalline grace, every reachable node is LIVE and unreachable to
// concurrent ops. Walk level 0, validate each node's canary, CAS
// LIVE -> INVALIDATED (which publishes ALERT_FIRED via
// SkiplistLinkTraits::fires_alert in the same atomic), and retire it.
// CAS-fail here means a writer reached a reachable node past grace —
// substrate-invariant break, trap.
void drain_arena_nodes(Arena *arena) {
    uint16_t enc =
        arena->head_next(0).load(cpp::MemoryOrder::ACQUIRE).next();
    while (enc != kLinkNullEncoding) {
        SkiplistNodeBase *node = resolve_link_target(enc);
        if (node == nullptr)
            break;

        // Validate node_canary before touching any retire-driving field —
        // a mismatch means post-init tampering.
        BucketGeometry geom = bucket_geometry(node->bucket);
        uint64_t expected_canary = compute_va_node_canary(
            partition_secret(), static_cast<uint16_t>(geom.cls),
            node->chunk_id, node->slot_idx);
        if (LIBC_UNLIKELY(node->node_canary != expected_canary))
            __builtin_trap();
        Link next = node->next[0].load(cpp::MemoryOrder::ACQUIRE);
        enc = next.next();
        node->owning_arena = nullptr;

        const bool cas_ok =
            link_cas_state<SkiplistNodeState::LIVE,
                           SkiplistNodeState::INVALIDATED,
                           SkiplistLinkTraits>(node->next[0]);
        if (LIBC_UNLIKELY(!cas_ok))
            __builtin_trap();
        g_va_tracker_skiplist_domain.retire(node);
    }
    arena->hint.store(nullptr, cpp::MemoryOrder::RELAXED);
    // Reset head links so the slot is reusable on release_slot_in_va_chunk.
    for (uint32_t lvl = 0; lvl < kMaxHeight; ++lvl) {
        arena->head_next(lvl).store(
            Link::pack(static_cast<uint8_t>(SkiplistNodeState::LIVE),
                       /*tag=*/0, kLinkNullEncoding),
            cpp::MemoryOrder::RELAXED);
    }
}

} // namespace

void arena_free(Arena *arena) {
    // Recover (cd, chunk_id, slot_idx) and triple-validate canaries before
    // touching any chunk-descriptor field — every external-pointer path
    // must clear corruption checks before reaching a real pool entry.
    PerArenaState &a = g_arena_state;
    RecoveredSlot rec = recover_slot_from_va(
        arena, kArenaChunkBytes, kArenaSlotSize, kArenaSlotsPerChunk,
        kChunksPerBucket, a.chunk_table);

    // arena_canary doubles as the per-slot node canary under the va_tracker
    // canary derivation.
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

// The inline sentinel head shares the per-slot canary derivation by
// construction, so the same value updates both. The LOCKED -> LIVE CAS
// scrubs any corrupted pre-fork state on the head's level-0 link; a head
// is never LOCKED in steady state, but a stale LOCKED bit would otherwise
// park future contenders indefinitely.
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
