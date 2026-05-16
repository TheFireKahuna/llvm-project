//===- interval_skiplist.cpp - Per-arena concurrent interval skiplist -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-arena interval skiplist; see the header for the design overview.
// Readers traverse under a Crystalline-W (Nikolaev & Ravindran, PLDI
// 2024) pin held in g_va_tracker_skiplist_domain. Logical deletion is
// the Harris (PODC 2001) mark bit on the predecessor's next link;
// physical unlink is a help-protocol CAS that any walker observing
// the mark may perform.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/memory/arena_alloc.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/skiplist_link_traits.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_addr.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

using ::LIBC_NAMESPACE::linkage::Link;
using ::LIBC_NAMESPACE::linkage::link_cas_set_mark;
using ::LIBC_NAMESPACE::linkage::link_cas_snap;
using ::LIBC_NAMESPACE::linkage::link_cas_snap_relink;
using ::LIBC_NAMESPACE::linkage::link_cas_state;
using ::LIBC_NAMESPACE::linkage::link_exchange_state_certify;
using ::LIBC_NAMESPACE::linkage::link_finalize_after_splice;
using ::LIBC_NAMESPACE::linkage::MarkedLinkSnap;
namespace partition_ns = alloc::partition;

//===----------------------------------------------------------------------===//
//  Compile-time layout assertions
//===----------------------------------------------------------------------===//
//
// Each bucket must hold a node tall enough for its maximum tower
// height; the slot range must fit in the chunk; chunk_bytes must match
// the pagemap's 64 KiB stamping granularity.

static_assert(bucket_geometry(0).slot_size >=
                  kSkiplistNodeHeaderBytes + 2 * sizeof(Link),
              "Bucket 0 slot must hold height-2 SkiplistNodeBase");
static_assert(bucket_geometry(1).slot_size >=
                  kSkiplistNodeHeaderBytes + 4 * sizeof(Link),
              "Bucket 1 slot must hold height-4 SkiplistNodeBase");
static_assert(bucket_geometry(2).slot_size >=
                  kSkiplistNodeHeaderBytes + 8 * sizeof(Link),
              "Bucket 2 slot must hold height-8 SkiplistNodeBase");
static_assert(bucket_geometry(3).slot_size >=
                  kSkiplistNodeHeaderBytes + 16 * sizeof(Link),
              "Bucket 3 slot must hold height-16 SkiplistNodeBase");

// Per-bucket slot range must fit inside the chunk, otherwise
// try_acquire_first_free_slot can hand out a slot whose tail spills
// past chunk_base + chunk_bytes.
static_assert(bucket_geometry(0).slot_size *
                  bucket_geometry(0).slots_per_chunk <=
                  bucket_geometry(0).chunk_bytes,
              "Bucket 0 slot range overflows chunk");
static_assert(bucket_geometry(1).slot_size *
                  bucket_geometry(1).slots_per_chunk <=
                  bucket_geometry(1).chunk_bytes,
              "Bucket 1 slot range overflows chunk");
static_assert(bucket_geometry(2).slot_size *
                  bucket_geometry(2).slots_per_chunk <=
                  bucket_geometry(2).chunk_bytes,
              "Bucket 2 slot range overflows chunk");
static_assert(bucket_geometry(3).slot_size *
                  bucket_geometry(3).slots_per_chunk <=
                  bucket_geometry(3).chunk_bytes,
              "Bucket 3 slot range overflows chunk");

// chunk_bytes must be an integer multiple of pagemap's 64 KiB stamping
// granularity — pagemap_register_range / pagemap_publish_range fan-write
// per-64-KiB entries and rely on the caller to round up.
static_assert(bucket_geometry(0).chunk_bytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "Bucket 0 chunk_bytes must be 64 KiB-aligned");
static_assert(bucket_geometry(1).chunk_bytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "Bucket 1 chunk_bytes must be 64 KiB-aligned");
static_assert(bucket_geometry(2).chunk_bytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "Bucket 2 chunk_bytes must be 64 KiB-aligned");
static_assert(bucket_geometry(3).chunk_bytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "Bucket 3 chunk_bytes must be 64 KiB-aligned");

// Partition guard size must itself be a 64 KiB multiple, otherwise
// `chunk_base = part->base + guard + cid*chunk_bytes` is misaligned for
// pagemap stamping even when `chunk_bytes` is a clean 64 KiB.
static_assert(partition_ns::kPartitionGuardBytes %
                  ::LIBC_NAMESPACE::windows::alloc::kPagemapChunkBytes == 0,
              "kPartitionGuardBytes must be 64 KiB-aligned so that "
              "chunk_base computed against the middle placeholder stays "
              "pagemap-aligned");

//===----------------------------------------------------------------------===//
//  Process-wide state
//===----------------------------------------------------------------------===//
//
// PCB Zone 1. The shared VaChunkDesc pool, pool-bitmap and drain helpers
// live in va_tracker_chunk.{h,cpp}; this TU owns only the per-bucket
// chunk_table allocator state plus the process-global g_link_chunk_table
// shared by all height buckets.

namespace {

PerBucketState g_bucket_state[kBucketCount];

// RELAXED throughout — pure monitoring, never synchronisation.
cpp::Atomic<uint32_t> g_nodes_allocated_per_bucket[kBucketCount]{};
cpp::Atomic<uint32_t> g_upper_publish_retry_count{0};

cpp::Atomic<uint32_t> g_init_done{0};

// One indexed atomic load resolves a successor — height buckets share
// the chunk-id namespace, so no four-bucket probe is needed.
cpp::Atomic<VaChunkDesc *> g_link_chunk_table[256]{};
cpp::Atomic<uint32_t> g_skiplist_chunk_id_hint{0};

// Duplicated per TU (also in va_tracker_chunk.cpp) — exposing it would
// force a function call on every alloc/free.
LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Every chunk-table load goes through
// g_va_tracker_skiplist_chunk_domain.protect(loc, idx, parent)
// (Nikolaev & Ravindran PLDI 2024, §4.2 Fig. 10). Without it a reader
// can observe a cd whose retire batch has already freed and whose pool
// slot now holds an unrelated descriptor (different chunk_base /
// slot_size / bucket_id) — the subsequent slot deref would then alias
// foreign pages.
//
// FreeFn sites (skiplist_node_free, region_desc_release, arena_free)
// don't need a fresh protected read: the slot they free has not yet
// been bitmap-cleared (mark_dead happens later in
// release_slot_in_va_chunk), so count_of(cd->live_state) > 0 and chunk
// drain is structurally blocked for the FreeFn's duration. That
// blocking depends on va_chunk_desc_free, NOT release_slot_in_va_chunk,
// performing the physical page decommit — moving decommit to drain
// would leave readers holding a valid cd whose chunk_base addresses
// already-freed pages.
constexpr uint32_t kPinSlotChunkDomain = 0;

// Reverse field checks (bucket/chunk_id/slot_idx) catch a stale enc
// that survived a chunk_table swap but addresses a slot now owned by
// another node.
[[nodiscard]] SkiplistNodeBase *resolve_link_target_impl(uint16_t enc) {
    if (enc == kLinkNullEncoding)
        return nullptr;
    uint8_t chunk_id = decode_link_chunk_id(enc);
    uint8_t slot_idx = decode_link_slot_idx(enc);
    VaChunkDesc *cd = g_va_tracker_skiplist_chunk_domain.protect(
        g_link_chunk_table[chunk_id], kPinSlotChunkDomain,
        /*parent=*/nullptr);
    if (cd == nullptr || cd == va_chunk_installing_sentinel() ||
        !is_live(cd->live_state))
        return nullptr;
    if (slot_idx >= cd->slot_capacity)
        return nullptr;

    uintptr_t base = reinterpret_cast<uintptr_t>(cd->chunk_base);
    uintptr_t slot_va =
        base + static_cast<uintptr_t>(slot_idx) *
                   static_cast<uintptr_t>(cd->slot_size);
    SkiplistNodeBase *node = reinterpret_cast<SkiplistNodeBase *>(slot_va);
    if (node->bucket != cd->bucket_id)
        return nullptr;
    if (node->chunk_id != chunk_id || node->slot_idx != slot_idx)
        return nullptr;
    return node;
}

} // namespace

// Cannot be defaulted: the moved-from LockedSet must be observably
// empty (valid()==false, arena==nullptr, errno_==0). The base-subobject
// move handles inline storage, overflow blocks and count; only the
// domain-specific fields need explicit clearing here.
LockedSet::LockedSet(LockedSet &&other) noexcept
    : Store(static_cast<Store &&>(other)), arena(other.arena),
      lo(other.lo), hi(other.hi), pred(other.pred),
      pred_snap(other.pred_snap), errno_(other.errno_) {
    other.arena = nullptr;
    other.pred = nullptr;
    other.errno_ = 0;
}

SkiplistNodeBase *resolve_link_target(uint16_t enc) {
    return resolve_link_target_impl(enc);
}

//===----------------------------------------------------------------------===//
//  Reader path — Crystalline-pinned chain decode
//===----------------------------------------------------------------------===//
//
// Two Crystalline-W domains cooperate. The skiplist-node domain pins
// SkiplistNodeBase bodies; the chunk domain (used only in
// resolve_link_target_impl) pins the underlying VaChunkDesc. The
// reservation tables are independent, so cross-domain re-entrancy is
// sound: one chain step pins the chunk descriptor, then the node body
// it resolves to. Slot indices are call-site discipline (the substrate
// does not enforce them); keeping the mapping fixed makes nested-
// reader pin lifetimes intelligible at a glance.

constexpr uint32_t kPinSlotPrev      = 0;
constexpr uint32_t kPinSlotCur       = 1;
constexpr uint32_t kPinSlotLevelPrev = 2;
constexpr uint32_t kPinSlotLevelCur  = 3;

namespace {

// Passed through protect()'s thunk overload. A foreign helper thread
// invokes the thunk on the helpee's behalf during slow-path helping,
// with parent_for_helper kept alive by the PROTECT2 / active-chain
// CAS scan.
struct SkiplistLinkLoadCtx {
    cpp::Atomic<Link> *link_field;
};

SkiplistNodeBase *skiplist_load_link_target_thunk(void *ctx_p) {
    auto *c = static_cast<SkiplistLinkLoadCtx *>(ctx_p);
    Link snap = c->link_field->load(cpp::MemoryOrder::ACQUIRE);
    uint16_t enc = snap.next();
    if (enc == kLinkNullEncoding)
        return nullptr;
    return resolve_link_target_impl(enc);
}

} // namespace

// One protect() call covers load, era convergence and bounded
// slow-path helping (Nikolaev & Ravindran PLDI 2024, §4.2 Fig. 10
// generalised form). Outside the anonymous namespace because the
// header's is_walk_range template calls it directly; body stays in
// this TU so resolve_link_target_impl's chunk-domain plumbing does.
[[nodiscard]] SkiplistNodeBase *
pinned_read_link_target(cpp::Atomic<Link> &link_field, uint32_t hr_idx,
                        SkiplistNodeBase *parent_for_helper) {
    SkiplistLinkLoadCtx ctx{&link_field};
    return g_va_tracker_skiplist_domain
        .protect<&skiplist_load_link_target_thunk>(ctx, hr_idx,
                                                    parent_for_helper);
}

} // namespace va_tracker
} // namespace windows

// Bodies need TU-local g_bucket_state and bucket geometry, so they
// cannot be inlined into the header; only the codec declarations are
// visible at the template instantiation site.
namespace concurrent {

uint32_t
BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::SkiplistNodeBase>::encode(
    CrystallineNode *n) noexcept {
  using Node = ::LIBC_NAMESPACE::windows::va_tracker::SkiplistNodeBase;
  auto *s = static_cast<Node *>(n);
  return 1u + ((static_cast<uint32_t>(s->bucket) << 16) |
               (static_cast<uint32_t>(s->chunk_id) << 8) |
               static_cast<uint32_t>(s->slot_idx));
}

CrystallineNode *
BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::SkiplistNodeBase>::decode(
    uint32_t code) noexcept {
  using Node = ::LIBC_NAMESPACE::windows::va_tracker::SkiplistNodeBase;
  uint32_t v = code - 1u;
  uint32_t bucket = (v >> 16) & 0xFFu;
  uint32_t chunk_id = (v >> 8) & 0xFFu;
  uint32_t slot_idx = v & 0xFFu;
  // slot_size lives in bucket geometry, not on the descriptor — the
  // flat decoder holds one cd per chunk_id, but stride is per-bucket.
  auto geom = ::LIBC_NAMESPACE::windows::va_tracker::bucket_geometry(bucket);
  auto *cd = ::LIBC_NAMESPACE::windows::va_tracker::g_link_chunk_table
                 [chunk_id]
                     .load(cpp::MemoryOrder::ACQUIRE);
  auto *base = static_cast<char *>(cd->chunk_base);
  return reinterpret_cast<Node *>(base + slot_idx * geom.slot_size);
}

// `BatchLinkCodec<Arena>` is defined in arena_alloc.cpp alongside
// `g_arena_state`; this TU sees it through the transitive reference in
// `g_va_tracker_arena_domain`.

} // namespace concurrent

namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
//  Crystalline-W domain instances
//===----------------------------------------------------------------------===//

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    SkiplistNodeBase, &skiplist_node_free, kSkiplistRetireFreq,
    kSkiplistMaxIdx>
    g_va_tracker_skiplist_domain;

// Records flagged cleanup_value_on_abort=false (e.g. caller's
// pre-allocated RegionDesc passed to is_insert) have their payload
// detached first so the FreeFn does not release caller-owned state on
// a retry.
void retire_unpublished_nodes(NewNodes &nodes) {
    for (uint32_t i = 0; i < nodes.count; ++i) {
        NewNodeRecord &rec = nodes.at(i);
        SkiplistNodeBase *node = rec.node;
        if (node == nullptr)
            continue;
        if (!rec.cleanup_value_on_abort)
            node->value.store(nullptr, cpp::MemoryOrder::RELAXED);
        g_va_tracker_skiplist_domain.retire(node);
        rec = NewNodeRecord{};
    }
    nodes.clear();
}

namespace {

//===----------------------------------------------------------------------===//
//  Chunk commit and slot release (skiplist height buckets)
//===----------------------------------------------------------------------===//

// The chunk_id space is process-global (Link::next's 8 bits are not
// per-bucket), so the installing-sentinel CAS on g_link_chunk_table is
// the single source of truth for "this id is taken".
[[nodiscard]] VaChunkDesc *commit_new_skiplist_chunk_for(uint8_t bucket_id,
                                                       BucketGeometry geom) {
    partition_ns::PartitionDescriptor *part =
        partition_ns::reserve_or_grow(geom.cls, partition_ns::kNodeAgnostic);
    if (LIBC_UNLIKELY(part == nullptr))
        return nullptr;

    PerBucketState &b = g_bucket_state[bucket_id];
    // Hint distributes contending threads across chunk_ids; RELAXED is
    // sufficient because the subsequent CAS on `g_link_chunk_table[cid]`
    // is the publish edge that establishes happens-before for whoever
    // wins.
    uint32_t hint =
        g_skiplist_chunk_id_hint.fetch_add(1, cpp::MemoryOrder::RELAXED) %
        kSkiplistAddressableChunks;

    for (uint32_t attempt = 0; attempt < kSkiplistAddressableChunks;
         ++attempt) {
        uint32_t cid = (hint + attempt) % kSkiplistAddressableChunks;
        VaChunkDesc *expected_global = nullptr;
        // CAS-as-install-lock: ACQ_REL on success pairs with the
        // RELEASE publish of the live cd below; ACQUIRE on failure
        // synchronises with another thread's publish. Exactly one
        // thread can claim cid until publish or rollback.
        if (!g_link_chunk_table[cid].compare_exchange_strong(
                expected_global, va_chunk_installing_sentinel(),
                cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE))
            continue;

        uintptr_t base = reinterpret_cast<uintptr_t>(part->base);
        uintptr_t chunk_va =
            base + partition_ns::kPartitionGuardBytes +
            static_cast<uintptr_t>(cid) *
                static_cast<uintptr_t>(geom.chunk_bytes);
        void *chunk_base = reinterpret_cast<void *>(chunk_va);

        // Claim the descriptor BEFORE committing pages: its pool index
        // becomes the pagemap entry's slot_idx, and commit_chunk
        // publishes the pagemap entry atomically with the commit.
        VaChunkDesc *cd = va_chunk_desc_pool_claim();
        if (cd == nullptr) {
            g_link_chunk_table[cid].store(nullptr,
                                          cpp::MemoryOrder::RELEASE);
            return nullptr;
        }
        uint32_t pool_idx = va_chunk_pool_index_of(cd);

        // Atomic: split + commit_replace + partition counter +
        // pagemap register + publish; rollback on failure.
        int rc = partition_ns::commit_chunk(
            part, chunk_base, geom.chunk_bytes, PAGE_READWRITE,
            ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer::VaTrackerVaChunk,
            pool_idx);
        if (rc != 0) {
            va_chunk_desc_pool_release(cd);
            g_link_chunk_table[cid].store(nullptr,
                                          cpp::MemoryOrder::RELEASE);
            if (rc == -EAGAIN)
                return nullptr;
            continue;
        }

        cd->chunk_base = chunk_base;
        cd->slot_size = geom.slot_size;
        cd->slot_capacity = geom.slots_per_chunk;
        cd->chunk_bytes = geom.chunk_bytes;
        cd->bucket_id = bucket_id;
        cd->chunk_id = static_cast<uint8_t>(cid);
        cd->partition = part;
        cd->chunk_canary = compute_va_chunk_canary(
            partition_secret(), static_cast<uint16_t>(geom.cls),
            static_cast<uint8_t>(cid));
        cd->occupancy.clear_all();
        // (state=Live, count=0, gen=0) packed into one 64-bit word so
        // release_va_chunk_slot's count-mutating CAS is single-atomic.
        init_live(cd->live_state);
        g_va_tracker_skiplist_chunk_domain.init_node(cd);

        // RELEASE-store replaces the installing sentinel. Readers that
        // pinned the sentinel re-converge via era inside protect() and
        // observe the live cd (or fail and restart).
        g_link_chunk_table[cid].store(cd, cpp::MemoryOrder::RELEASE);
        uint32_t next = (cid + 1) % kSkiplistAddressableChunks;
        b.next_chunk_id_hint.store(next, cpp::MemoryOrder::RELAXED);
        return cd;
    }
    return nullptr;
}

} // namespace

namespace {

struct SkiplistInitCtx {
    uint8_t bucket_id;
    uint8_t height;
    Arena *owning_arena;
    partition_ns::PartitionClass cls;
};

// Runs after va_chunk_acquire_slot has zeroed the slot and validated
// canaries. next[i] stores are RELAXED because the slot is not yet
// visible — publish happens through the caller's CAS into a reachable
// predecessor.
void skiplist_init_slot(void *slot, VaChunkDesc * /*cd*/,
                        uint32_t chunk_id, uint32_t slot_idx, void *ctx_p) {
    auto *ctx = static_cast<SkiplistInitCtx *>(ctx_p);
    auto *node = static_cast<SkiplistNodeBase *>(slot);
    node->bucket = ctx->bucket_id;
    node->chunk_id = static_cast<uint8_t>(chunk_id);
    node->slot_idx = static_cast<uint8_t>(slot_idx);
    node->height = ctx->height;
    node->owning_arena = ctx->owning_arena;
    node->node_canary = compute_va_node_canary(
        partition_secret(), static_cast<uint16_t>(ctx->cls),
        node->chunk_id, node->slot_idx);
    g_va_tracker_skiplist_domain.init_node(node);
    for (uint32_t i = 0; i < ctx->height; ++i) {
        node->next[i].store(
            Link::pack(static_cast<uint8_t>(SkiplistNodeState::LIVE),
                       /*tag=*/0, kLinkNullEncoding),
            cpp::MemoryOrder::RELAXED);
    }
    g_nodes_allocated_per_bucket[ctx->bucket_id].fetch_add(
        1, cpp::MemoryOrder::RELAXED);
}

} // namespace

SkiplistNodeBase *bucket_alloc_node(uint8_t height, Arena *owning_arena) {
    if (LIBC_UNLIKELY(height == 0 || height > kMaxHeight))
        __builtin_trap();
    if (LIBC_UNLIKELY(owning_arena == nullptr))
        __builtin_trap();

    uint8_t bucket_id = bucket_for_height(height);
    BucketGeometry geom = bucket_geometry(bucket_id);
    PerBucketState &b = g_bucket_state[bucket_id];

    SkiplistInitCtx ctx{bucket_id, height, owning_arena, geom.cls};
    VaChunkAcquireSpec spec{
        /*cls=*/geom.cls,
        /*chunk_table=*/g_link_chunk_table,
        /*next_chunk_id_hint=*/&b.next_chunk_id_hint,
        /*chunk_count=*/kSkiplistAddressableChunks,
        /*slots_per_chunk=*/kSlotsPerChunk,
        /*consumer_bucket_id=*/bucket_id,
        /*init=*/&skiplist_init_slot,
        /*init_ctx=*/&ctx,
    };

    for (;;) {
        void *slot = va_chunk_acquire_slot(spec);
        if (slot != nullptr)
            return static_cast<SkiplistNodeBase *>(slot);

        VaChunkDesc *cd = commit_new_skiplist_chunk_for(bucket_id, geom);
        if (cd == nullptr)
            return nullptr;
    }
}

//===----------------------------------------------------------------------===//
//  Harris mark protocol — logical deletion and help-splice
//===----------------------------------------------------------------------===//
//
// Logical deletion is the Harris (PODC 2001) mark on the predecessor's
// next link. A walker observing MARK on cur->next[lvl] finalises the
// splice by CAS-replacing prev->next[lvl] with cur's successor, then
// runs link_finalize_after_splice to convert cur's mark to
// cert+unmark. Strong CAS at both sides serialises the original marker
// against any number of concurrent helpers.

namespace {

// Honours the inline head sentinel's split storage (head.next[0] for
// level 0, head_tail_links_ for levels >= 1).
[[nodiscard]] LIBC_INLINE cpp::Atomic<Link> &
node_next(Arena *arena, SkiplistNodeBase *node, uint32_t level) {
    return node == &arena->head ? arena->head_next(level) : node->next[level];
}

void help_unlink_marked(SkiplistNodeBase *prev, SkiplistNodeBase *cur,
                        Arena *arena, uint32_t level) {
    cpp::Atomic<Link> &prev_next = node_next(arena, prev, level);
    Link prev_link = prev_next.load(cpp::MemoryOrder::ACQUIRE);
    uint16_t cur_enc = encode_link_next(cur->chunk_id, cur->slot_idx);
    if (prev_link.next() != cur_enc)
        return;  // already spliced

    Link cur_link = cur->next[level].load(cpp::MemoryOrder::ACQUIRE);
    if (!cur_link.is_marked())
        return;  // no longer marked

    // Pivot past cur. with_next_uncertify drops CERT/MARK/ALERT because
    // the new successor inherits a fresh edge; state byte is preserved
    // (prev is on its own state machine).
    uint16_t new_next = cur_link.next();
    Link desired = prev_link.with_next_uncertify(new_next);
    if (!prev_next.compare_exchange_strong(
            prev_link, desired, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
        return;  // raced; another walker will help

    // Convert cur's mark to cert+unmark — Crystalline's retire
    // predicate refuses still-marked nodes.
    MarkedLinkSnap mks = MarkedLinkSnap::from_observed_marked(cur_link);
    (void)link_finalize_after_splice(cur->next[level], mks);
}

} // namespace

//===----------------------------------------------------------------------===//
//  Find predecessor — level-0 search anchor for Lock and Alloc
//===----------------------------------------------------------------------===//
//
// Returns the last node whose hi <= lo (or the head sentinel) plus a
// snap of its next[0] for downstream CAS. `restart` requests a fresh
// outer retry on chain-integrity violations or markers we cannot
// safely race past.

namespace {

struct PredLookupResult {
    SkiplistNodeBase *pred{nullptr};
    Link pred_snap{};
    bool restart{false};
};

[[nodiscard]] PredLookupResult find_predecessor(Arena *arena, uintptr_t lo) {
    PredLookupResult plr;

    SkiplistNodeBase *prev = &arena->head;
    g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);

    for (uint32_t lvl = kMaxHeight; lvl > 0;) {
        --lvl;
        uint32_t steps = 0;
        for (;;) {
            if (++steps > kTraversalStepLimit)
                __builtin_trap();
            cpp::Atomic<Link> &prev_link = node_next(arena, prev, lvl);
            Link prev_snap = prev_link.load(cpp::MemoryOrder::ACQUIRE);
            if (prev_snap.is_marked()) {
                plr.restart = true;
                return plr;
            }
            uint16_t cur_enc = prev_snap.next();
            if (cur_enc == kLinkNullEncoding)
                break;

            SkiplistNodeBase *cur =
                pinned_read_link_target(prev_link, kPinSlotCur, prev);
            if (cur == nullptr) {
                plr.restart = true;
                return plr;
            }
            if (cur->height <= lvl) {
                plr.restart = true;
                return plr;
            }

            Link cur_snap = cur->next[lvl].load(cpp::MemoryOrder::ACQUIRE);
            if (cur_snap.is_marked()) {
                help_unlink_marked(prev, cur, arena, lvl);
                continue;
            }

            uint8_t cur_state = cur_snap.state();
            if (cur_state ==
                static_cast<uint8_t>(SkiplistNodeState::INVALIDATED)) {
                plr.restart = true;
                return plr;
            }
            if (cur_state == static_cast<uint8_t>(SkiplistNodeState::IDLE))
                continue;

            if (cur->hi > lo)
                break;

            prev = cur;
            g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);
        }
    }

    plr.pred = prev;
    plr.pred_snap = node_next(arena, prev, 0).load(cpp::MemoryOrder::ACQUIRE);
    return plr;
}

//===----------------------------------------------------------------------===//
//  GETPREDSUCC — per-level bookmark gather for Swap and Alloc
//===----------------------------------------------------------------------===//
//
// Single top-down pass; each level reuses the previous level's
// bookmark as its start point, yielding O(log N + spans) per Swap
// rather than O(log N) per level (paper §A pseudocode lines 4-31).
//
// Only level-0 prev is pinned on exit; upper-level bookmark pointers
// are raw, used as optimistic CAS hints. If a bookmarked pred is
// retired and its slot recycled, the publish CAS fails on tag
// mismatch (T1) and the caller breaks per paper §A line 81; T3
// (never-freed pool VA) makes the raw deref at worst a stale read,
// never a fault.

struct LevelBookmark {
    SkiplistNodeBase *pred{nullptr};
    Link              pred_snap{};
};

struct LevelBookmarks {
    LevelBookmark per_level[kMaxHeight]{};
    bool          restart{false};
};

[[nodiscard]] LevelBookmarks
gather_level_bookmarks(Arena *arena, uintptr_t lo) {
    LevelBookmarks out;

    SkiplistNodeBase *prev = &arena->head;
    g_va_tracker_skiplist_domain.anchor(kPinSlotLevelPrev);

    for (uint32_t lvl = kMaxHeight; lvl > 0;) {
        --lvl;
        uint32_t steps = 0;
        for (;;) {
            if (++steps > kTraversalStepLimit)
                __builtin_trap();
            cpp::Atomic<Link> &prev_link = node_next(arena, prev, lvl);
            Link prev_snap = prev_link.load(cpp::MemoryOrder::ACQUIRE);
            if (prev_snap.is_marked()) {
                out.restart = true;  // peer mid-splice on prev
                return out;
            }
            uint16_t cur_enc = prev_snap.next();
            if (cur_enc == kLinkNullEncoding) {
                out.per_level[lvl] = {prev, prev_snap};
                break;
            }

            SkiplistNodeBase *cur =
                pinned_read_link_target(prev_link, kPinSlotLevelCur, prev);
            if (cur == nullptr) {
                out.restart = true;
                return out;
            }
            if (cur->height <= lvl) {
                // Chain integrity: level i must only hold height > i.
                out.restart = true;
                return out;
            }

            Link cur_snap = cur->next[lvl].load(cpp::MemoryOrder::ACQUIRE);
            if (cur_snap.is_marked()) {
                help_unlink_marked(prev, cur, arena, lvl);
                continue;
            }
            uint8_t cur_state = cur_snap.state();
            if (cur_state ==
                static_cast<uint8_t>(SkiplistNodeState::INVALIDATED)) {
                out.restart = true;
                return out;
            }
            if (cur_state == static_cast<uint8_t>(SkiplistNodeState::IDLE))
                continue;

            if (cur->hi > lo) {
                // prev is the bookmark at this level.
                out.per_level[lvl] = {prev, prev_snap};
                break;
            }

            // Carrying prev down across level descents is what keeps
            // the cumulative cost O(log N).
            prev = cur;
            g_va_tracker_skiplist_domain.anchor(kPinSlotLevelPrev);
        }
    }
    return out;
}

} // namespace

//===----------------------------------------------------------------------===//
//  Lock acquisition — paper Algorithm 1
//===----------------------------------------------------------------------===//
//
// One link_cas_state<LIVE, LOCKED, SkiplistLinkTraits> per node does
// triple duty: linearises the lock, bumps the tag (closes in-place
// ABA), and via the traits hook sets LINK_ALERT_FIRED_BIT for future
// parkers. The parking word and the lock state share one atomic.
//
// On observed LOCKED on a successor we release the predecessor LOCK
// before parking so disjoint intervals make progress while we sleep
// (paper §4.4); the successor's release CAS wakes us.

bool LockedSet::acquire(Arena *arena_in, uintptr_t lo_in, uintptr_t hi_in) {
    // Defer-acquire: callers carry a default-constructed slot and
    // populate it in place. Overwriting a populated slot leaks its
    // locks, so trap.
    if (LIBC_UNLIKELY(pred != nullptr || arena != nullptr || !is_unused()))
        __builtin_trap();

    LockedSet &set = *this;
    set.arena = arena_in;
    set.lo = lo_in;
    set.hi = hi_in;
    Arena *arena = arena_in;
    uintptr_t lo = lo_in;
    uintptr_t hi = hi_in;

    if (LIBC_UNLIKELY(arena == nullptr || lo >= hi)) {
        set.errno_ = -EINVAL;
        return false;
    }

    for (;;) {
        PredLookupResult plr = find_predecessor(arena, lo);
        if (plr.restart || plr.pred == nullptr)
            continue;

        SkiplistNodeBase *pred = plr.pred;
        Link pred_snap = plr.pred_snap;
        uint8_t pred_state = pred_snap.state();

        if (pred_state ==
            static_cast<uint8_t>(SkiplistNodeState::INVALIDATED)) {
            continue; // restart
        }
        if (pred_state == static_cast<uint8_t>(SkiplistNodeState::LOCKED)) {
            // Park on the link word. Holder's release CAS fires ALERT_FIRED
            // via traits (hint bit only); the paired `futex_addr::wake`
            // delivers the kernel-level `NtAlertThreadByThreadId` that
            // actually wakes us.
            uint64_t snap_value = pred_snap.raw();
            auto *raw =
                reinterpret_cast<volatile uint64_t *>(&pred->next[0].val);
            (void)::LIBC_NAMESPACE::futex_addr::wait<uint64_t,
                                                       /*Interruptible=*/false>(
                raw, snap_value, /*timeout=*/nullptr);
            continue;
        }

        LIBC_ASSERT(pred_state ==
                        static_cast<uint8_t>(SkiplistNodeState::LIVE) &&
                    "Lock: predecessor must be LIVE pre-CAS");

        // Predecessor lock linearisation.
        if (!link_cas_snap<SkiplistNodeState::LOCKED, SkiplistLinkTraits>(
                pred->next[0], pred_snap)) {
            continue; // tag mismatch / state changed under us
        }

        // LOCKED makes pred structurally retire-blocked; the pin is
        // still needed so any peer that observes the link and tries to
        // help sees a live parent.
        g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);

        set.pred = pred;
        set.pred_snap = pred->next[0].load(cpp::MemoryOrder::ACQUIRE);
        set.clear();

        bool restart = false;
        bool park_succ = false;
        Link park_succ_snap{};
        SkiplistNodeBase *park_succ_node = nullptr;

        SkiplistNodeBase *walk_prev = pred;
        uint16_t cur_enc = set.pred_snap.next();
        while (cur_enc != kLinkNullEncoding) {
            cpp::Atomic<Link> &walk_prev_link = walk_prev->next[0];
            SkiplistNodeBase *cur =
                pinned_read_link_target(walk_prev_link, kPinSlotCur, walk_prev);
            if (cur == nullptr) {
                restart = true;
                break;
            }
            if (cur->lo >= hi)
                break; // no more overlap

            Link cur_snap = cur->next[0].load(cpp::MemoryOrder::ACQUIRE);
            if (cur_snap.is_marked()) {
                help_unlink_marked(walk_prev, cur, arena, /*level=*/0);
                restart = true;
                break;
            }
            uint8_t cur_state = cur_snap.state();
            if (cur_state ==
                static_cast<uint8_t>(SkiplistNodeState::INVALIDATED)) {
                restart = true;
                break;
            }
            if (cur_state ==
                static_cast<uint8_t>(SkiplistNodeState::LOCKED)) {
                park_succ = true;
                park_succ_snap = cur_snap;
                park_succ_node = cur;
                break;
            }
            LIBC_ASSERT(cur_state ==
                        static_cast<uint8_t>(SkiplistNodeState::LIVE));

            if (!link_cas_snap<SkiplistNodeState::LOCKED, SkiplistLinkTraits>(
                    cur->next[0], cur_snap)) {
                restart = true;
                break;
            }

            if (!set.push(cur)) {
                // Storage exhausted. Undo cur's LOCK and surface
                // -ENOMEM; the release CAS fires the alert and the
                // explicit wake delivers it.
                (void)link_cas_state<SkiplistNodeState::LOCKED,
                                      SkiplistNodeState::LIVE,
                                      SkiplistLinkTraits>(cur->next[0]);
                ::LIBC_NAMESPACE::futex_addr::wake(
                    reinterpret_cast<volatile void *>(&cur->next[0].val),
                    0xFFFFFFFFu);
                set.errno_ = -ENOMEM;
                restart = true;
                break;
            }

            // Slot Cur is overwritten on the next pinned_read_link_target;
            // slot Prev is re-anchored to stay era-current.
            walk_prev = cur;
            g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);

            cur_snap = cur->next[0].load(cpp::MemoryOrder::ACQUIRE);
            cur_enc = cur_snap.next();
        }

        if (restart) {
            Unlock(set, /*include_pred=*/true);
            set.pred = nullptr;
            if (set.errno_ == -ENOMEM)
                return false;
            set.clear();
            continue;
        }

        if (park_succ) {
            // Paper §4.4 disjoint-region parallelism: release all our
            // holds before sleeping so disjoint mutators can proceed.
            Unlock(set, /*include_pred=*/true);
            set.pred = nullptr;
            set.clear();

            uint64_t snap_value = park_succ_snap.raw();
            auto *raw = reinterpret_cast<volatile uint64_t *>(
                &park_succ_node->next[0].val);
            (void)::LIBC_NAMESPACE::futex_addr::wait<uint64_t,
                                                       /*Interruptible=*/false>(
                raw, snap_value, /*timeout=*/nullptr);
            continue;
        }

        return true;
    }
}

//===----------------------------------------------------------------------===//
//  Unlock — release LOCKED, fire alerts, wake parkers
//===----------------------------------------------------------------------===//
//
// The traits hook flips LINK_ALERT_FIRED_BIT atomically with the LOCKED
// -> LIVE transition, but ALERT_FIRED is a hint bit only — the actual
// kernel wake is NtAlertThreadByThreadId, issued by the explicit
// futex_addr::wake below.

void Unlock(LockedSet &set, bool include_pred) {
    for (uint32_t i = 0; i < set.count; ++i) {
        SkiplistNodeBase *n = set.at(i);
        (void)link_cas_state<SkiplistNodeState::LOCKED, SkiplistNodeState::LIVE,
                              SkiplistLinkTraits>(n->next[0]);
        ::LIBC_NAMESPACE::futex_addr::wake(
            reinterpret_cast<volatile void *>(&n->next[0].val), 0xFFFFFFFFu);
    }
    if (include_pred && set.pred) {
        (void)link_cas_state<SkiplistNodeState::LOCKED, SkiplistNodeState::LIVE,
                              SkiplistLinkTraits>(set.pred->next[0]);
        ::LIBC_NAMESPACE::futex_addr::wake(
            reinterpret_cast<volatile void *>(&set.pred->next[0].val),
            0xFFFFFFFFu);
    }
    set.clear();
}

namespace {

void mark_upper_level(SkiplistNodeBase *old_node, uint32_t lvl) {
    for (;;) {
        Link cur = old_node->next[lvl].load(cpp::MemoryOrder::ACQUIRE);
        // Marked already (lazy walker beat us) or certified (a helper
        // splice has converted mark -> cert+unmark): the node is
        // proven off-chain at this level, no re-mark needed.
        if (cur.is_marked() || cur.is_certified())
            return;
        if (link_cas_set_mark(old_node->next[lvl], cur))
            return;
    }
}

[[nodiscard]] bool locked_run_contains_level_enc(LockedSet &set,
                                                 uint32_t lvl,
                                                 uint16_t enc) {
    if (enc == kLinkNullEncoding)
        return false;
    for (uint32_t i = 0; i < set.count; ++i) {
        SkiplistNodeBase *old_node = set.at(i);
        if (old_node->height <= lvl)
            continue;
        if (encode_link_next(old_node->chunk_id, old_node->slot_idx) == enc)
            return true;
    }
    return false;
}

[[nodiscard]] uint16_t old_level_successor(LockedSet &set, uint32_t lvl) {
    for (uint32_t i = set.count; i > 0;) {
        --i;
        SkiplistNodeBase *old_node = set.at(i);
        if (old_node->height <= lvl)
            continue;
        Link snap = old_node->next[lvl].load(cpp::MemoryOrder::ACQUIRE);
        return snap.next();
    }
    return kLinkNullEncoding;
}

void finalize_marked_old_level(LockedSet &set, uint32_t lvl) {
    for (uint32_t i = 0; i < set.count; ++i) {
        SkiplistNodeBase *old_node = set.at(i);
        if (old_node->height <= lvl)
            continue;
        Link snap = old_node->next[lvl].load(cpp::MemoryOrder::ACQUIRE);
        if (snap.is_marked()) {
            (void)link_finalize_after_splice(
                old_node->next[lvl],
                MarkedLinkSnap::from_observed_marked(snap));
        }
    }
}

//===----------------------------------------------------------------------===//
//  Swap helpers — upper-level mark, batch unlink, new-run stitching
//===----------------------------------------------------------------------===//
//
// Paper §4.3 "collective decrement" (Fig. 8b/c): mark every old node's
// upper-level outgoing link top-down, then per-level batch-unlink keyed
// off the gather-pass bookmarks. On CAS failure we leave the marks and
// rely on lazy walkers to splice — the level-0 linearisation has
// already committed, so the result is observably correct; missing skip
// links only degrade subsequent searches to slower (still correct)
// traversal.

// One eager-unlink attempt at upper level lvl. with_next_uncertify
// drops CERT/MARK/ALERT (pred is on-chain, its prior CERT was 0; ALERT
// is owner-irrelevant on a non-set predecessor).
[[nodiscard]] bool
try_unlink_old_run_at_level(LockedSet &set, LevelBookmark &bookmark,
                            uint32_t lvl, uint16_t succ_enc) {
    if (bookmark.pred == nullptr)
        return false;

    Arena *arena = set.arena;
    cpp::Atomic<Link> &pred_next = node_next(arena, bookmark.pred, lvl);
    Link expected = bookmark.pred_snap;
    Link desired = expected.with_next_uncertify(succ_enc);
    bool cas_ok = pred_next.compare_exchange_strong(
        expected, desired, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::ACQUIRE);
    if (cas_ok) {
        // Refresh snap for the upcoming publish CAS, then convert
        // marks to cert+unmark on every old member at this level.
        bookmark.pred_snap = pred_next.load(cpp::MemoryOrder::ACQUIRE);
        finalize_marked_old_level(set, lvl);
        return true;
    }
    // Refresh the stale snap so the publish phase can reuse it.
    bookmark.pred_snap = pred_next.load(cpp::MemoryOrder::ACQUIRE);
    return false;
}

// Paper Fig. 8b Phase 1. Runs before the level-0 linearisation:
// Crystalline retirement requires the retired node to be unreachable
// from every upper level, and lazy walkers may not have spliced yet.
void mark_old_set_upper_levels(LockedSet &set) {
    for (uint32_t i = 0; i < set.count; ++i) {
        SkiplistNodeBase *old_node = set.at(i);
        for (uint32_t lvl = old_node->height; lvl > 1;) {
            --lvl;
            mark_upper_level(old_node, lvl);
        }
    }
}

void unlink_upper_levels_before_retire(LockedSet &set,
                                        LevelBookmarks &bookmarks) {
    mark_old_set_upper_levels(set);
    for (uint32_t lvl = kMaxHeight; lvl > 1;) {
        --lvl;
        uint16_t succ_enc = old_level_successor(set, lvl);
        if (succ_enc == kLinkNullEncoding &&
            !locked_run_contains_level_enc(
                set, lvl, bookmarks.per_level[lvl].pred_snap.next())) {
            // No old member has height > lvl — nothing to unlink.
            continue;
        }
        // kLinkNullEncoding succ_enc is a valid pivot target when the
        // last set member at this level had no successor.
        (void)try_unlink_old_run_at_level(set, bookmarks.per_level[lvl],
                                           lvl, succ_enc);
    }
}

// Level 0 stays LOCKED on every new node until the level-0 publish
// completes — otherwise a concurrent overlap update could retire one of
// these nodes mid-tower-publish.
struct NewLevelRuns {
    SkiplistNodeBase *left[kMaxHeight]{};
    SkiplistNodeBase *right[kMaxHeight]{};
    uint32_t max_height{0};
};

[[nodiscard]] NewLevelRuns prepare_new_level_runs(NewNodes &new_nodes) {
    NewLevelRuns runs;
    for (uint32_t nidx = 0; nidx < new_nodes.count; ++nidx) {
        SkiplistNodeBase *node = new_nodes.node_at(nidx);
        if (runs.max_height < node->height)
            runs.max_height = node->height;
        uint16_t enc = encode_link_next(node->chunk_id, node->slot_idx);
        for (uint32_t lvl = 0; lvl < node->height; ++lvl) {
            if (runs.left[lvl] == nullptr)
                runs.left[lvl] = node;
            if (runs.right[lvl] != nullptr) {
                runs.right[lvl]->next[lvl].store(
                    Link::pack(
                        static_cast<uint8_t>(
                            lvl == 0 ? SkiplistNodeState::LOCKED
                                     : SkiplistNodeState::LIVE),
                        /*tag=*/0, enc),
                    cpp::MemoryOrder::RELAXED);
            }
            runs.right[lvl] = node;
        }
    }
    return runs;
}

void terminate_new_level0_run(NewLevelRuns &runs, uint16_t tail_succ_enc) {
    if (runs.right[0] != nullptr) {
        runs.right[0]->next[0].store(
            Link::pack(static_cast<uint8_t>(SkiplistNodeState::LOCKED),
                        /*tag=*/0, tail_succ_enc),
            cpp::MemoryOrder::RELAXED);
    }
}

void release_published_new_nodes(NewNodes &new_nodes) {
    for (uint32_t i = 0; i < new_nodes.count; ++i) {
        SkiplistNodeBase *node = new_nodes.node_at(i);
        (void)link_cas_state<SkiplistNodeState::LOCKED,
                              SkiplistNodeState::LIVE,
                              SkiplistLinkTraits>(node->next[0]);
        ::LIBC_NAMESPACE::futex_addr::wake(
            reinterpret_cast<volatile void *>(&node->next[0].val),
            0xFFFFFFFFu);
    }
}

// Paper Fig. 18 lines 78-81, Fig. 8e/f. Abandons remaining levels on
// first CAS failure (paper §A line 81 break semantics): the level-0
// linearisation has already committed so the Swap is correct; missing
// skip links only degrade traversal until a future op rebalances.
//
// bookmarks has been mutated in place by unlink_upper_levels_*, so
// bookmark.pred_snap.next() is the post-old-set successor encoding.
void publish_new_upper_levels(Arena *arena, NewLevelRuns &runs,
                              LevelBookmarks &bookmarks) {
    for (uint32_t lvl = 1; lvl < runs.max_height; ++lvl) {
        SkiplistNodeBase *left = runs.left[lvl];
        if (left == nullptr)
            continue;
        LevelBookmark &bookmark = bookmarks.per_level[lvl];
        if (bookmark.pred == nullptr) {
            g_upper_publish_retry_count.fetch_add(
                1, cpp::MemoryOrder::RELAXED);
            return; // paper-correct break
        }
        uint16_t left_enc = encode_link_next(left->chunk_id, left->slot_idx);
        // RELAXED store: runs.right[lvl] is owned exclusively by us
        // until the publish CAS below makes it observable on the chain.
        runs.right[lvl]->next[lvl].store(
            Link::pack(static_cast<uint8_t>(SkiplistNodeState::LIVE),
                        /*tag=*/0, bookmark.pred_snap.next()),
            cpp::MemoryOrder::RELAXED);
        cpp::Atomic<Link> &pred_next =
            node_next(arena, bookmark.pred, lvl);
        if (!link_cas_snap_relink<SkiplistNodeState::LIVE,
                                   SkiplistLinkTraits>(
                pred_next, bookmark.pred_snap, left_enc)) {
            g_upper_publish_retry_count.fetch_add(
                1, cpp::MemoryOrder::RELAXED);
            return; // paper-correct break
        }
    }
}

} // namespace

//===----------------------------------------------------------------------===//
//  Swap — paper Algorithm 2 (linearisation point of the mutation)
//===----------------------------------------------------------------------===//
//
// Linearisation point: a single link_cas_snap_relink<LIVE,
// SkiplistLinkTraits> on pred->next[0] transitions LOCKED -> LIVE AND
// pivots next to the new chain head in one atomic. Before that CAS the
// old set must be unreachable from every upper level (Crystalline
// retirement requires it); after it, the new run's upper-level
// publishes splice in lazily and the old nodes are stamped INVALIDATED
// and retired.
//
// Returnable soft failures:
//   * set.pred == nullptr — caller-side invariant violation defence.
//   * bookmarks.restart   — peer mid-splice / chain integrity issue at
//                           upper levels; caller's Map retries Lock.
//
// The Step-3 linearisation CAS is NOT a returnable failure: pred is
// LOCKED from Lock-acquire through Step 3, and the substrate has no
// write site that can modify pred->next[0] between the ACQUIRE load at
// Step 3 and the matching compare_exchange_strong. Concurrent peer
// Locks against a LOCKED pred fail their LIVE->LOCKED CAS without
// modifying the link word; help_unlink_marked at level 0 cannot fire
// because no caller marks next[0] (mark_upper_level is lvl > 1 only,
// and no other site in this TU sets a level-0 mark). Steps 1's writes
// target lvl > 1 only. The Step-3 CAS is therefore guaranteed to
// succeed, and a failure indicates a substrate-invariant violation —
// trap-on-failure surfaces it immediately, instead of dropping into
// the caller's rollback path with kernel state that may not be
// recoverable for the PrivateCommit OW case. Symmetric with Step 5's
// already-trapping LOCKED->INVALIDATED CAS on the old set.

bool Swap(LockedSet &set, NewNodes &new_nodes) {
    if (LIBC_UNLIKELY(set.pred == nullptr))
        return false;

    Arena *arena = set.arena;

    // Step 0. Single top-down GETPREDSUCC capturing pred bookmarks at
    // every level — keeps Swap at O(log N) rather than O(N log N).
    LevelBookmarks bookmarks = gather_level_bookmarks(arena, set.lo);
    if (bookmarks.restart)
        return false;  // peer mutation; caller's Map retries Lock

    // Step 1. Detach old nodes from every level > 0; the level-0
    // linearisation in step 3 requires this to have observably
    // completed. On per-level CAS failure marks remain and a lazy
    // walker splices.
    unlink_upper_levels_before_retire(set, bookmarks);

    NewLevelRuns runs;
    if (!new_nodes.empty())
        runs = prepare_new_level_runs(new_nodes);

    // Step 2. Stitch the new chain off-list. Upper-level tails are
    // filled immediately before each upper publish CAS, AFTER step 3.
    uint16_t tail_succ_enc;
    if (set.count > 0) {
        // Last locked succ's next[0] already points past the locked
        // range to the first non-locked node.
        SkiplistNodeBase *last_locked = set.at(set.count - 1);
        Link last_link = last_locked->next[0].load(cpp::MemoryOrder::ACQUIRE);
        tail_succ_enc = last_link.next();
    } else {
        // Pure-insert / no overlap: tail is whatever followed pred.
        tail_succ_enc = set.pred_snap.next();
    }

    if (!new_nodes.empty())
        terminate_new_level0_run(runs, tail_succ_enc);

    // Step 3. Linearisation CAS.
    uint16_t new_head_enc;
    if (!new_nodes.empty()) {
        SkiplistNodeBase *first = runs.left[0];
        new_head_enc = encode_link_next(first->chunk_id, first->slot_idx);
    } else {
        new_head_enc = tail_succ_enc;
    }

    Link pred_now = set.pred->next[0].load(cpp::MemoryOrder::ACQUIRE);
    // Lock established LOCKED and nothing in steps 0-2 releases the
    // pred-side LOCK.
    LIBC_ASSERT(pred_now.state() ==
                static_cast<uint8_t>(SkiplistNodeState::LOCKED));

    // CAS failure is a substrate-invariant violation; see the Swap
    // banner for the no-concurrent-writer audit.
    if (LIBC_UNLIKELY(!link_cas_snap_relink<SkiplistNodeState::LIVE,
                                              SkiplistLinkTraits>(
            set.pred->next[0], pred_now, new_head_enc))) {
        __builtin_trap();
    }
    // Release CAS fired ALERT_FIRED via the traits hook; pair with the
    // kernel wake.
    ::LIBC_NAMESPACE::futex_addr::wake(
        reinterpret_cast<volatile void *>(&set.pred->next[0].val), 0xFFFFFFFFu);

    // Step 4. Multi-level upper publish (paper Fig. 8). New level-0
    // nodes stay LOCKED until this completes — otherwise a concurrent
    // overlap update could retire one mid-tower-publish.
    if (!new_nodes.empty()) {
        publish_new_upper_levels(arena, runs, bookmarks);
        release_published_new_nodes(new_nodes);
    }

    // Step 5. Stamp every old node INVALIDATED. Typed link_cas_state
    // (not link_exchange_state) is mandatory — the runtime variant
    // skips the traits fires_alert hook, leaving ALERT_FIRED un-set
    // and breaking the contract for any reader still parked on a stale
    // LOCKED snap. Every old node is guaranteed LOCKED on entry
    // (nothing between Lock and here releases it), so CAS failure
    // means the invariant was violated — trap.
    for (uint32_t i = 0; i < set.count; ++i) {
        SkiplistNodeBase *old_node = set.at(i);
        // Bool temp: LIBC_UNLIKELY is single-arg and cannot consume a
        // comma-bearing template-id directly.
        const bool cas_ok =
            link_cas_state<SkiplistNodeState::LOCKED,
                           SkiplistNodeState::INVALIDATED,
                           SkiplistLinkTraits>(old_node->next[0]);
        if (LIBC_UNLIKELY(!cas_ok))
            __builtin_trap();
        ::LIBC_NAMESPACE::futex_addr::wake(
            reinterpret_cast<volatile void *>(&old_node->next[0].val),
            0xFFFFFFFFu);
    }

    // Step 5b. Retire old node descriptors. Backing kernel-state
    // teardown is closed synchronously by the enclosing transaction's
    // post-Swap reaper, so this is metadata-only — the FreeFn touches
    // no kernel handles.
    //
    // The locked set is intentionally NOT cleared here: the post-Swap
    // survivor walk relies on walking set.at(i) to discover OLD
    // backings that need teardown. Callers MUST Unlock to clear. The
    // Banakar 2025 Map template path performs no such walk and clears
    // at the call site immediately after a successful Swap to preserve
    // the destructor's empty-store invariant.
    for (uint32_t i = 0; i < set.count; ++i) {
        g_va_tracker_skiplist_domain.retire(set.at(i));
    }

    return true;
}

//===----------------------------------------------------------------------===//
//  Query — wait-free point lookup
//===----------------------------------------------------------------------===//
//
// Top-down level descent under a skiplist-domain pin. INVALIDATED
// nodes remain readable per paper I5 (value valid until grace, gated
// by the caller's pin). AS-safe / VEH-callable: every internal load
// goes through pinned_read_link_target so a fault on a recycled slot
// is impossible.

RegionDesc *Query(Arena *arena, uintptr_t key,
                  SkiplistNodeBase **out_node) {
    if (out_node)
        *out_node = nullptr;
    if (LIBC_UNLIKELY(arena == nullptr))
        return nullptr;
    if (key < arena->arena_lo || key >= arena->arena_hi)
        return nullptr;

    SkiplistNodeBase *prev = &arena->head;
    g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);

    // kTraversalStepLimit is the structural ceiling on distinct nodes
    // (8-bit chunk_id × 256 slots = 65535, plus height slack);
    // exceeding it means chain corruption (cycle or runaway help) — trap.
    for (uint32_t lvl = kMaxHeight; lvl > 0;) {
        --lvl;
        uint32_t steps = 0;
        for (;;) {
            if (++steps > kTraversalStepLimit)
                __builtin_trap();
            cpp::Atomic<Link> &prev_link =
                (prev == &arena->head ? arena->head_next(lvl)
                                      : prev->next[lvl]);
            SkiplistNodeBase *cur =
                pinned_read_link_target(prev_link, kPinSlotCur, prev);
            if (cur == nullptr)
                break;
            if (cur->height <= lvl)
                break; // chain integrity defense

            Link cur_snap = cur->next[lvl].load(cpp::MemoryOrder::ACQUIRE);
            if (cur_snap.is_marked()) {
                help_unlink_marked(prev, cur, arena, lvl);
                continue;
            }
            // IDLE is unreachable here under the pin (substrate T2).
            // maybe_unused for release builds (no -Wunused-variable).
            [[maybe_unused]] const uint8_t st = cur_snap.state();
            LIBC_ASSERT(st == static_cast<uint8_t>(SkiplistNodeState::LIVE) ||
                        st == static_cast<uint8_t>(SkiplistNodeState::LOCKED) ||
                        st == static_cast<uint8_t>(SkiplistNodeState::INVALIDATED));
            if (cur->hi <= key) {
                prev = cur;
                g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);
                continue;
            }
            break;  // cur extends past key; descend a level
        }
    }
    // prev is now the largest node with hi <= key (or the head).
    uint32_t steps = 0;
    for (;;) {
        if (++steps > kTraversalStepLimit)
            __builtin_trap();
        cpp::Atomic<Link> &prev_link =
            (prev == &arena->head ? arena->head_next(0) : prev->next[0]);
        SkiplistNodeBase *cur =
            pinned_read_link_target(prev_link, kPinSlotCur, prev);
        if (cur == nullptr)
            return nullptr;
        if (cur->lo > key)
            return nullptr;
        if (cur->hi > key) {
            // INVALIDATED OK: value held until grace, grace gated by
            // the caller's pin (paper I5).
            if (out_node)
                *out_node = cur;
            return cur->value.load(cpp::MemoryOrder::ACQUIRE);
        }
        prev = cur;
        g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);
    }
}

//===----------------------------------------------------------------------===//
//  Alloc — paper "Alloc" surface (gap-find + insert-CAS)
//===----------------------------------------------------------------------===//
//
// Insertion is a single-node Swap: prepare an off-list candidate at
// pred's post-snapshot, CAS-pivot pred to it, run the upper-level
// publish. arena->hint routes a two-pass scan (probe past the hint
// first, fall back to the full range).

namespace {

// Lock-contention waiter shape factored for Alloc's CAS-fail retries.
void wait_on_level0_link(cpp::Atomic<Link> &link, Link snap) {
    uint64_t snap_value = snap.raw();
    auto *raw = reinterpret_cast<volatile uint64_t *>(&link.val);
    (void)::LIBC_NAMESPACE::futex_addr::wait<uint64_t,
                                             /*Interruptible=*/false>(
        raw, snap_value, /*timeout=*/nullptr);
}

[[nodiscard]] LIBC_INLINE uintptr_t max_addr(uintptr_t a, uintptr_t b) {
    return a > b ? a : b;
}

[[nodiscard]] LIBC_INLINE uintptr_t min_addr(uintptr_t a, uintptr_t b) {
    return a < b ? a : b;
}

[[nodiscard]] bool add_overflow(uintptr_t a, size_t b, uintptr_t *out) {
    uintptr_t ub = static_cast<uintptr_t>(b);
    if (a > ~uintptr_t{0} - ub)
        return true;
    *out = a + ub;
    return false;
}

} // namespace

uintptr_t Alloc(Arena *arena, uintptr_t lo, uintptr_t hi, size_t len,
                RegionDesc *value) {
    if (LIBC_UNLIKELY(arena == nullptr || lo >= hi || len == 0 ||
                      value == nullptr))
        return 0;

    uintptr_t bound_lo = max_addr(lo, arena->arena_lo);
    uintptr_t bound_hi = min_addr(hi, arena->arena_hi);
    if (bound_lo >= bound_hi)
        return 0;
    if (static_cast<uintptr_t>(len) > bound_hi - bound_lo)
        return 0;

    uintptr_t starts[2]{bound_lo, bound_lo};
    uint32_t pass_count = 1;
    SkiplistNodeBase *hint = g_va_tracker_skiplist_domain.protect(
        arena->hint, kPinSlotCur, &arena->head);
    if (hint != nullptr && hint->owning_arena == arena &&
        hint->hi > bound_lo && hint->hi < bound_hi) {
        starts[0] = hint->hi;
        starts[1] = bound_lo;
        pass_count = 2;
    }

    SkiplistNodeBase *candidate = nullptr;
    for (uint32_t pass = 0; pass < pass_count; ++pass) {
        uintptr_t pass_lo = starts[pass];
        uintptr_t pass_hi = pass == 1 ? starts[0] : bound_hi;
        if (pass_lo >= pass_hi ||
            static_cast<uintptr_t>(len) > pass_hi - pass_lo)
            continue;

        for (;;) {
            PredLookupResult plr = find_predecessor(arena, pass_lo);
            if (plr.restart || plr.pred == nullptr)
                continue;

            SkiplistNodeBase *pred = plr.pred;
            g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);

            bool restart = false;
            while (!restart) {
                cpp::Atomic<Link> &pred_next = node_next(arena, pred, 0);
                Link pred_snap = pred_next.load(cpp::MemoryOrder::ACQUIRE);
                uint8_t pred_state = pred_snap.state();
                if (pred_state ==
                    static_cast<uint8_t>(SkiplistNodeState::LOCKED)) {
                    wait_on_level0_link(pred_next, pred_snap);
                    restart = true;
                    break;
                }
                if (pred_state == static_cast<uint8_t>(
                                      SkiplistNodeState::INVALIDATED) ||
                    pred_state ==
                        static_cast<uint8_t>(SkiplistNodeState::IDLE)) {
                    restart = true;
                    break;
                }

                uint16_t observed_next = pred_snap.next();
                SkiplistNodeBase *next = nullptr;
                if (observed_next != kLinkNullEncoding) {
                    next = pinned_read_link_target(pred_next, kPinSlotCur, pred);
                    if (next == nullptr) {
                        restart = true;
                        break;
                    }
                }
                uintptr_t gap_start =
                    pred == &arena->head ? pass_lo
                                         : max_addr(pred->hi, pass_lo);
                uintptr_t gap_end = pass_hi;

                if (next != nullptr) {
                    Link next_snap =
                        next->next[0].load(cpp::MemoryOrder::ACQUIRE);
                    if (next_snap.is_marked()) {
                        help_unlink_marked(pred, next, arena, 0);
                        continue;
                    }
                    uint8_t next_state = next_snap.state();
                    if (next_state == static_cast<uint8_t>(
                                          SkiplistNodeState::INVALIDATED) ||
                        next_state ==
                            static_cast<uint8_t>(SkiplistNodeState::IDLE)) {
                        restart = true;
                        break;
                    }
                    gap_end = min_addr(next->lo, pass_hi);
                }

                uintptr_t alloc_hi = 0;
                if (gap_start < gap_end &&
                    !add_overflow(gap_start, len, &alloc_hi) &&
                    alloc_hi <= gap_end) {
                    if (candidate == nullptr) {
                        candidate =
                            bucket_alloc_node(sample_node_height(), arena);
                        if (candidate == nullptr)
                            return 0;
                    }

                    candidate->lo = gap_start;
                    candidate->hi = alloc_hi;
                    candidate->value.store(value, cpp::MemoryOrder::RELEASE);
                    candidate->next[0].store(
                        Link::pack(
                            static_cast<uint8_t>(SkiplistNodeState::LOCKED),
                            /*tag=*/0, pred_snap.next()),
                        cpp::MemoryOrder::RELAXED);

                    uint16_t self_enc = encode_link_next(
                        candidate->chunk_id, candidate->slot_idx);
                    if (link_cas_snap_relink<SkiplistNodeState::LIVE,
                                             SkiplistLinkTraits>(
                            pred_next, pred_snap, self_enc)) {
                        // Linearisation won. Fresh GETPREDSUCC pass
                        // promotes the candidate's upper levels.
                        NewNodes singleton;
                        if (!singleton.push(
                                candidate,
                                /*cleanup_value_if_unpublished=*/false))
                            __builtin_trap();
                        NewLevelRuns runs = prepare_new_level_runs(singleton);
                        if (runs.max_height > 1) {
                            LevelBookmarks alloc_bookmarks =
                                gather_level_bookmarks(arena, gap_start);
                            if (!alloc_bookmarks.restart)
                                publish_new_upper_levels(arena, runs,
                                                          alloc_bookmarks);
                        }
                        release_published_new_nodes(singleton);
                        arena->hint.store(candidate,
                                          cpp::MemoryOrder::RELEASE);
                        return gap_start;
                    }

                    Link after_fail =
                        pred_next.load(cpp::MemoryOrder::ACQUIRE);
                    if (after_fail.state() == static_cast<uint8_t>(
                                                  SkiplistNodeState::LOCKED)) {
                        wait_on_level0_link(pred_next, after_fail);
                        restart = true;
                        break;
                    }
                    if (after_fail.state() == static_cast<uint8_t>(
                                                  SkiplistNodeState::INVALIDATED)) {
                        restart = true;
                        break;
                    }
                    if (after_fail.state() ==
                        static_cast<uint8_t>(SkiplistNodeState::IDLE)) {
                        restart = true;
                        break;
                    }
                    // Gap claimed under us or pred-link shifted; stay
                    // on this predecessor (paper Alloc CAS-fail case 3).
                    continue;
                }

                if (next == nullptr || next->lo >= pass_hi)
                    break;
                pred = next;
                g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);
            }
            if (!restart)
                break;
        }
    }

    if (candidate != nullptr) {
        candidate->value.store(nullptr, cpp::MemoryOrder::RELAXED);
        g_va_tracker_skiplist_domain.retire(candidate);
    }
    return 0;
}

//===----------------------------------------------------------------------===//
//  Tower-height PRNG and visitor wrappers
//===----------------------------------------------------------------------===//

namespace {

// xorshift64 (Marsaglia, J. Stat. Soft. 2003) → geometric on
// [1, kMaxHeight]. Per-thread TLS seeded from
// tid ^ pid ^ partition_secret defends against inserter correlation.
// All-zero is the "needs reseed" sentinel; the seeder substitutes a
// golden-ratio fallback if the entropy mix lands on zero.
//
// sample_height_fork_reseed clears the surviving thread post-fork so
// the next sample mixes the rotated secret + fresh pid; new child
// threads start at zero-init and reseed on first use.
thread_local uint64_t g_height_prng_state = 0;

[[nodiscard]] LIBC_INLINE uint64_t sample_height_seed_from_environment() {
    uint64_t tid = static_cast<uint64_t>(::NtCurrentThreadId());
    uint64_t pid = static_cast<uint64_t>(::NtCurrentProcessId());
    uint64_t s = (tid << 32) ^ pid ^ partition_secret();
    if (LIBC_UNLIKELY(s == 0))
        s = 0x9E3779B97F4A7C15ULL; // golden-ratio fallback
    return s;
}

} // namespace

[[nodiscard]] uint8_t sample_node_height() {
    uint64_t s = g_height_prng_state;
    if (LIBC_UNLIKELY(s == 0))
        s = sample_height_seed_from_environment();
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    g_height_prng_state = s;

    uint8_t h = 1;
    while (h < kMaxHeight && (s & 1) != 0) {
        ++h;
        s >>= 1;
    }
    return h;
}

void sample_height_fork_reseed() {
    // The surviving thread's TID is unchanged across fork, so entropy
    // would come entirely from secret+pid; clearing forces a reseed
    // that mixes the rotated secret.
    g_height_prng_state = 0;
}

//===----------------------------------------------------------------------===//
//  Insert and erase visitors — driven by Map
//===----------------------------------------------------------------------===//
//
// Fragment helpers (append_left_fragment / append_right_fragment)
// preserve the surviving prefix/suffix on partial overlap by cloning
// the old RegionDesc and pointing the new node at the shifted backing
// offset.

namespace {

[[nodiscard]] int append_interval_node(NewNodes &out, Arena *arena,
                                       uintptr_t lo, uintptr_t hi,
                                       RegionDesc *value,
                                       bool cleanup_value_on_abort) {
    if (lo >= hi)
        return 0;

    SkiplistNodeBase *n = bucket_alloc_node(sample_node_height(), arena);
    if (n == nullptr)
        return -ENOMEM;

    n->lo = lo;
    n->hi = hi;
    n->value.store(value, cpp::MemoryOrder::RELEASE);
    if (!out.push(n, cleanup_value_on_abort)) {
        n->value.store(nullptr, cpp::MemoryOrder::RELAXED);
        g_va_tracker_skiplist_domain.retire(n);
        return -ENOMEM;
    }
    return 0;
}

[[nodiscard]] int append_cloned_fragment(NewNodes &out, Arena *arena,
                                         SkiplistNodeBase *old_node,
                                         uintptr_t frag_lo,
                                         uintptr_t frag_hi) {
    if (frag_lo >= frag_hi)
        return 0;

    RegionDesc *old_desc =
        old_node->value.load(cpp::MemoryOrder::ACQUIRE);
    RegionDesc *new_desc =
        clone_region_desc_for_fragment(old_desc, old_node->lo, frag_lo);
    if (new_desc == nullptr)
        return -ENOMEM;

    int err = append_interval_node(out, arena, frag_lo, frag_hi, new_desc,
                                   /*cleanup_value_on_abort=*/true);
    if (err != 0)
        region_desc_release(new_desc);
    return err;
}

[[nodiscard]] int append_left_fragment(NewNodes &out, Arena *arena,
                                       LockedSet &locked, uintptr_t lo) {
    for (uint32_t i = 0; i < locked.count; ++i) {
        SkiplistNodeBase *old_node = locked.at(i);
        if (old_node->lo >= lo)
            return 0;
        uintptr_t frag_hi = old_node->hi < lo ? old_node->hi : lo;
        if (old_node->lo < frag_hi)
            return append_cloned_fragment(out, arena, old_node,
                                          old_node->lo, frag_hi);
    }
    return 0;
}

[[nodiscard]] int append_right_fragment(NewNodes &out, Arena *arena,
                                        LockedSet &locked, uintptr_t hi) {
    for (uint32_t i = 0; i < locked.count; ++i) {
        SkiplistNodeBase *old_node = locked.at(i);
        if (old_node->hi <= hi)
            continue;
        uintptr_t frag_lo = old_node->lo > hi ? old_node->lo : hi;
        if (frag_lo < old_node->hi)
            return append_cloned_fragment(out, arena, old_node,
                                          frag_lo, old_node->hi);
        return 0;
    }
    return 0;
}

struct InsertVisitorImpl {
    RegionDesc *value{nullptr};
    Arena *arena{nullptr};

    LIBC_INLINE VisitorOutcome operator()(LockedSet &locked, uintptr_t lo,
                                            uintptr_t hi) {
        VisitorOutcome out;
        if (value == nullptr) {
            out.err = -EINVAL;
            return out;
        }

        out.err = append_left_fragment(out.nodes, arena, locked, lo);
        uint32_t insert_idx = out.nodes.count;
        if (out.err == 0)
            out.err = append_interval_node(
                out.nodes, arena, lo, hi, value,
                /*cleanup_value_on_abort=*/false);
        if (out.err == 0)
            out.err = append_right_fragment(out.nodes, arena, locked, hi);
        if (out.err == 0) {
            out.update_hint = true;
            out.hint_after_success = out.nodes.node_at(insert_idx);
        }
        if (out.err != 0)
            retire_unpublished_nodes(out.nodes);
        return out;
    }
};

struct EraseVisitorImpl {
    Arena *arena{nullptr};

    LIBC_INLINE VisitorOutcome operator()(LockedSet &locked,
                                            uintptr_t lo,
                                            uintptr_t hi) {
        VisitorOutcome out;
        out.err = append_left_fragment(out.nodes, arena, locked, lo);
        if (out.err == 0)
            out.err = append_right_fragment(out.nodes, arena, locked, hi);
        if (out.err == 0) {
            out.update_hint = true;
            out.hint_after_success = locked.pred;
        }
        if (out.err != 0)
            retire_unpublished_nodes(out.nodes);
        return out;
    }
};

} // namespace

int is_insert(Arena *arena, uintptr_t lo, uintptr_t hi, RegionDesc *value) {
    InsertVisitorImpl v;
    v.value = value;
    v.arena = arena;
    return Map(arena, lo, hi, v);
}

int is_erase(Arena *arena, uintptr_t lo, uintptr_t hi) {
    EraseVisitorImpl v;
    v.arena = arena;
    return Map(arena, lo, hi, v);
}

//===----------------------------------------------------------------------===//
//  Crystalline-W FreeFn — metadata-only retirement
//===----------------------------------------------------------------------===//
//
// Async when the last pin on a retired node drains. Touches no NT
// handles — kernel-state lifecycle is owned by the enclosing
// Transaction, closed synchronously before commit.

void skiplist_node_free(SkiplistNodeBase *node) {
    if (LIBC_UNLIKELY(node == nullptr))
        __builtin_trap();

    if (LIBC_UNLIKELY(node->bucket == 0xFF))
        __builtin_trap();

    // Validate node canary BEFORE chunk dereference — catches
    // use-after-free that survived the substrate Safety Triad.
    BucketGeometry geom = bucket_geometry(node->bucket);
    uint64_t expected_node_canary = compute_va_node_canary(
        partition_secret(),
        static_cast<uint16_t>(geom.cls),
        node->chunk_id, node->slot_idx);
    if (LIBC_UNLIKELY(node->node_canary != expected_node_canary))
        __builtin_trap();

    VaChunkDesc *cd = g_link_chunk_table[node->chunk_id].load(
        cpp::MemoryOrder::ACQUIRE);
    if (LIBC_UNLIKELY(cd == nullptr ||
                      cd == va_chunk_installing_sentinel()))
        __builtin_trap();

    uint64_t expected_chunk_canary = compute_va_chunk_canary(
        partition_secret(), static_cast<uint16_t>(geom.cls), node->chunk_id);
    if (LIBC_UNLIKELY(cd->chunk_canary != expected_chunk_canary))
        __builtin_trap();

    // Substrate T2 retire-Retry gate. INVALIDATED -> IDLE plus CERT in
    // one atomic; a reader racing through this link observes IDLE and
    // rejects the slot before recycle.
    (void)link_exchange_state_certify(
        node->next[0], static_cast<uint8_t>(SkiplistNodeState::IDLE));

    Arena *arena = node->owning_arena;
    if (arena) {
        SkiplistNodeBase *expected_hint = node;
        arena->hint.compare_exchange_strong(
            expected_hint, nullptr, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE);
    }

    // The commit-retiring Transaction already closed any backing's
    // kernel handles synchronously; this is metadata-only desc release.
    RegionDesc *rd = node->value.load(cpp::MemoryOrder::RELAXED);
    if (rd != nullptr) {
        node->value.store(nullptr, cpp::MemoryOrder::RELAXED);
        region_desc_release(rd);
    }

    release_slot_in_va_chunk(cd, node->slot_idx, g_link_chunk_table);
}

//===----------------------------------------------------------------------===//
//  Init and fork-reinit
//===----------------------------------------------------------------------===//

void interval_skiplist_init() {
    if (g_init_done.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;

    va_chunk_init();
    arena_init_registration();
    va_chunk_desc_pool_init_once();
    // g_bucket_state and g_link_chunk_table zero-init through BSS;
    // RegionDesc / Arena per-class state needs no explicit setup.
}

// Walk a chunk's occupancy bitmap and dispatch to a visitor for every
// set bit. Single-threaded post-fork, so RELAXED reads suffice.
// Function-pointer dispatch (no lambdas) for debuggability and
// consistency with the rest of the libc fork path.

namespace {

using ForkScrubVisitor = void (*)(uint32_t slot, void *slot_ptr,
                                    uint16_t cls_id, uint8_t chunk_id);

void fork_scrub_chunk_bitmap(VaChunkDesc *cd, uint32_t cap_bits,
                              ForkScrubVisitor visit, uint16_t cls_id,
                              uint8_t chunk_id) {
    constexpr uint32_t kBitsPerWord = 64;
    const uint32_t word_count =
        (cap_bits + kBitsPerWord - 1) / kBitsPerWord;
    uintptr_t base = reinterpret_cast<uintptr_t>(cd->chunk_base);
    for (uint32_t w = 0; w < word_count; ++w) {
        uint64_t bits = cd->occupancy.template word_at<
            cpp::MemoryOrder::RELAXED>(w);
        const uint32_t word_lo = w * kBitsPerWord;
        if (word_lo + kBitsPerWord > cap_bits) {
            const uint32_t valid = cap_bits - word_lo;
            bits &= (valid == kBitsPerWord)
                        ? ~uint64_t{0}
                        : ((uint64_t{1} << valid) - 1);
        }
        while (bits != 0) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t slot = word_lo + bit;
            void *slot_ptr = reinterpret_cast<void *>(
                base + static_cast<uintptr_t>(slot) *
                           static_cast<uintptr_t>(cd->slot_size));
            visit(slot, slot_ptr, cls_id, chunk_id);
        }
    }
}

// Refresh node canary, then scrub any LOCKED state on level-0. LOCKED
// only ever appears on level 0 (paper §3 Lock CAS targets
// pred->next[0]); the pre-fork lock owner is gone, so future acquirers
// would park forever without this scrub.
void fork_scrub_visit_skiplist_node(uint32_t slot, void *slot_ptr,
                                      uint16_t cls_id, uint8_t chunk_id) {
    auto *node = static_cast<SkiplistNodeBase *>(slot_ptr);
    node->node_canary = compute_va_node_canary(
        partition_secret(), cls_id, chunk_id, static_cast<uint8_t>(slot));
    (void)link_cas_state<SkiplistNodeState::LOCKED, SkiplistNodeState::LIVE,
                          SkiplistLinkTraits>(node->next[0]);
}

//===----------------------------------------------------------------------===//
//  Fork leaked-descriptor reclaim
//===----------------------------------------------------------------------===//
//
// Pre-fork dead threads' Crystalline cells held retire batches whose
// FreeFn would return the VaChunkDesc to the pool and decommit chunk
// pages. In the child those threads are gone, so the FreeFn never
// runs — the pool slot leaks and chunk pages stay committed forever.
//
// Crystalline stays fork-unaware. The reclaim walks every owned
// chunk_table to compute the reachable set, then walks the pool and
// reclaims any claimed descriptor in our buckets that is unreachable.
// Bounded by pre-fork in-flight retire cardinality; safe because the
// reachable set is built from chunk_table snapshots at fork when no
// peer thread can republish.

struct ForkReclaimCtx {
    // Set bit = reachable from one of our chunk_tables, do not reclaim.
    static constexpr uint32_t kBitsetWords =
        (kTotalVaChunkDescPoolSize + 63) / 64;
    uint64_t reachable_bits[kBitsetWords];
};

LIBC_INLINE void mark_reachable(ForkReclaimCtx &ctx, VaChunkDesc *cd) {
    if (cd == nullptr)
        return;
    uint32_t idx = va_chunk_pool_index_of(cd);
    ctx.reachable_bits[idx / 64] |= uint64_t{1} << (idx % 64);
}

// Adapter so RegionDesc / Arena fork-reinit phases can contribute their
// reachable descriptors through one uniform callback signature.
void mark_reachable_visit(VaChunkDesc *cd, void *ctx_p) {
    auto *ctx = static_cast<ForkReclaimCtx *>(ctx_p);
    mark_reachable(*ctx, cd);
}

// Reclaims only this subsystem's buckets — DescBacking (6) and ART
// (7..10) are owned by their own fork hooks so a single pool walk
// doesn't duplicate work.
void fork_reclaim_visit(VaChunkDesc *cd, void *ctx_p) {
    auto *ctx = static_cast<ForkReclaimCtx *>(ctx_p);
    if (cd->bucket_id != kPoolBucketSkiplist1_2 &&
        cd->bucket_id != kPoolBucketSkiplist3_4 &&
        cd->bucket_id != kPoolBucketSkiplist5_8 &&
        cd->bucket_id != kPoolBucketSkiplist9_16 &&
        cd->bucket_id != kPoolBucketRegionDesc &&
        cd->bucket_id != kPoolBucketArena)
        return;
    uint32_t idx = va_chunk_pool_index_of(cd);
    if (ctx->reachable_bits[idx / 64] & (uint64_t{1} << (idx % 64)))
        return;  // reachable

    if (cd->partition != nullptr && cd->chunk_base != nullptr) {
        partition_ns::decommit_chunk(cd->partition, cd->chunk_base,
                                     cd->chunk_bytes);
    }
    mark_dead(cd->live_state);
    va_chunk_desc_pool_release(cd);
}

} // namespace

void interval_skiplist_fork_reinit() {
    g_va_tracker_skiplist_domain.clear_all();
    va_chunk_fork_reinit();

    // Must run after va_chunk_fork_reinit so the next sample mixes the
    // rotated partition_secret rather than the pre-fork one.
    sample_height_fork_reseed();

    // Per chunk_table, two passes per chunk: refresh chunk_canary
    // against the rotated secret, then walk occupancy for per-slot
    // canary refresh + LOCKED scrub. The walk also marks reachable
    // descriptors for the leaked-descriptor reclaim below. Skipping
    // the canary pass would leave the next FreeFn trapping on
    // node_canary != compute_va_node_canary(new_secret, ...).
    //
    // Skips the in-progress install sentinel (low-address non-pointer;
    // never a valid descriptor).
    ForkReclaimCtx reclaim_ctx{};
    for (uint32_t cid = 0; cid < kSkiplistAddressableChunks; ++cid) {
        VaChunkDesc *cd =
            g_link_chunk_table[cid].load(cpp::MemoryOrder::ACQUIRE);
        if (cd == nullptr || cd == va_chunk_installing_sentinel())
            continue;
        mark_reachable(reclaim_ctx, cd);
        uint16_t cls_id =
            static_cast<uint16_t>(bucket_geometry(cd->bucket_id).cls);
        cd->chunk_canary = compute_va_chunk_canary(
            partition_secret(), cls_id, static_cast<uint8_t>(cid));
        fork_scrub_chunk_bitmap(cd, kSlotsPerChunk,
                                 &fork_scrub_visit_skiplist_node, cls_id,
                                 static_cast<uint8_t>(cid));
    }
    region_desc_fork_reinit_phase(&mark_reachable_visit, &reclaim_ctx);
    arena_fork_reinit_phase(&mark_reachable_visit, &reclaim_ctx);

    for_each_claimed_va_chunk_desc(&fork_reclaim_visit, &reclaim_ctx);
}

//===----------------------------------------------------------------------===//
//  Diagnostics
//===----------------------------------------------------------------------===//

SkiplistStats stats_snapshot() {
    SkiplistStats s{};
    for (uint32_t b = 0; b < kBucketCount; ++b)
        s.nodes_allocated_per_bucket[b] =
            g_nodes_allocated_per_bucket[b].load(cpp::MemoryOrder::RELAXED);
    // Skips null and the in-progress install sentinel.
    for (uint32_t cid = 0; cid < kSkiplistAddressableChunks; ++cid) {
        VaChunkDesc *cd =
            g_link_chunk_table[cid].load(cpp::MemoryOrder::ACQUIRE);
        if (cd == nullptr || cd == va_chunk_installing_sentinel())
            continue;
        if (cd->bucket_id < kBucketCount)
            ++s.live_chunks_per_bucket[cd->bucket_id];
    }
    s.arena_count = arena_live_count();
    s.upper_publish_retry_count =
        g_upper_publish_retry_count.load(cpp::MemoryOrder::RELAXED);
    return s;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
