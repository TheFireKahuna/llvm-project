//===- interval_skiplist.cpp - Per-arena concurrent interval skiplist -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-arena interval skiplist: clean-room reproduction of Kim, Kwon and Kang
// (SOSP 2025). One instance per (ART-leaf, CPU) pair holds the
// address-range -> RegionDesc map for the va_tracker. Per-node interval-
// scoped locks ride in the `state` byte of `next[0].val`; the same atomic
// that linearises the lock acquisition also covers the parking word, so
// disjoint VA intervals proceed without lock contention. Contention parks
// via `futex_addr::wait` directly on the 64-bit link word; release-class
// state CAS sites fire alerts through `SkiplistLinkTraits::fires_alert`
// and a paired `futex_addr::wake` issues `NtAlertThreadByThreadId`.
//
// Readers traverse under a Crystalline-W (Nikolaev & Ravindran, PLDI 2024)
// pin held in `g_va_tracker_skiplist_domain`. Logical deletion is the
// Harris (PODC 2001) mark bit on the predecessor's `next` link; physical
// unlink is a help-protocol CAS that any walker observing the mark may
// perform.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/memory/arena_alloc.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk_state.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_primitives_bootstrap.h"
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
// Each height bucket must hold a node tall enough for its maximum tower
// height, the slot range must fit in the chunk, and chunk_bytes must
// match the pagemap's 64 KiB stamping granularity. Violations are catch-
// at-build-time rather than first-fault traps in the substrate.

// `kSkiplistNodeHeaderBytes` (= 72) is the byte offset of the trailing
// `next[]` flexible array; the derivation avoids `__builtin_offsetof`
// on the non-standard-layout `CrystallineNode` base.
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

// Per-bucket slot range must fit inside the chunk; otherwise
// `try_acquire_first_free_slot` could hand out a slot whose tail spills
// past `chunk_base + chunk_bytes`.
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

// Each bucket's chunk size must be an integer multiple of the pagemap's
// 64 KiB stamping granularity — `pagemap_register_range` /
// `pagemap_publish_range` fan-write per-64-KiB entries and rely on the
// caller to round up.
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
// PCB Zone 1 (mutable). The shared `VaChunkDesc` pool, pool-bitmap and
// chunk-state-machine drain helpers live in `va_tracker_chunk.{h,cpp}`;
// this TU owns only the per-bucket chunk_table allocator state plus the
// process-global `g_link_chunk_table` direct decoder shared by all height
// buckets.

namespace {

// Per-bucket allocator state. Process-lifetime; CoW-inherited across fork.
PerBucketState g_bucket_state[kBucketCount];

// Per-process diagnostic counters. Always RELAXED — values are pure
// monitoring, never synchronisation.
cpp::Atomic<uint32_t> g_nodes_allocated_per_bucket[kBucketCount]{};
cpp::Atomic<uint32_t> g_upper_publish_retry_count{0};

// One-shot init guard for the Tier A bring-up sequence.
cpp::Atomic<uint32_t> g_init_done{0};

// Direct decoder for the encoded `Link::next` field. Skiplist height
// buckets share the process-global chunk-id namespace, so a successor
// load is one indexed atomic load rather than a four-bucket probe.
cpp::Atomic<VaChunkDesc *> g_link_chunk_table[256]{};
cpp::Atomic<uint32_t> g_skiplist_chunk_id_hint{0};

// Per-NODE canary derivation reads `partition_secret` from PCB Zone 0b,
// which `va_tracker_chunk.cpp` also caches under its own translation unit.
// The helper is duplicated rather than exposed because each TU needs its
// own inlining target — making it linkage-visible would force a function
// call on every alloc/free.
LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

// Chunk-domain reservation discipline.
//
// `g_va_tracker_skiplist_chunk_domain` is the Crystalline-W domain that
// owns `VaChunkDesc` body lifetime. A reader loading a `cd` pointer from
// `chunk_table[cid]` without participating in the domain's reservation
// grid can observe `cd` after its retire batch has freed and the pool
// slot has been recycled as an unrelated descriptor (different
// chunk_base / slot_size / bucket_id) — the subsequent slot dereference
// then aliases foreign pages.
//
// Every chunk-table load on this TU therefore goes through
// `g_va_tracker_skiplist_chunk_domain.protect(loc, idx, parent)`
// (Nikolaev & Ravindran PLDI 2024, §4.2 Fig. 10). The call atomically
// loads `*loc` while converging the cell era so the returned pointer's
// `birth_era <= cell.era`; any subsequent retire of that pointer
// publishes a batch whose `min_birth <= cell.era`, attaching it to this
// cell and deferring `va_chunk_desc_free` until we drain on the next
// `protect`/`clear_all`.
//
// FreeFn sites (`skiplist_node_free`, `region_desc_release`,
// `arena_free`) do not need a fresh protected read: the slot they are
// freeing has not yet been bitmap-cleared (mark_dead happens later in
// `release_slot_in_va_chunk`), so `count_of(cd->live_state) > 0` and
// chunk drain is structurally blocked for the FreeFn's duration.
//
// `va_chunk_desc_free` performs the physical page decommit, not
// `release_slot_in_va_chunk`. If decommit ran on drain rather than on
// FreeFn, `protect()` would correctly defer the descriptor's pool-slot
// recycle yet leave readers holding a valid `cd` pointer whose
// `chunk_base` addresses already-freed pages.
constexpr uint32_t kPinSlotChunkDomain = 0;

// Resolve an encoded `Link::next` value via the process-global
// `g_link_chunk_table`. The chunk-domain `protect()` pins `cd` for the
// duration of the dereference; reverse-direction field checks
// (`bucket`/`chunk_id`/`slot_idx`) catch a stale `enc` that survived a
// chunk_table swap but addresses a slot now owned by another node.
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

// The move ctor cannot be defaulted because the moved-from LockedSet must
// be observably empty (`valid()` false, `arena == nullptr`,
// `errno_ == 0`). The base-subobject move handles the inline storage,
// overflow blocks and count; the domain-specific fields are cleared here.
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
// Two Crystalline-W domains cooperate on every reader walk. The
// skiplist-node domain (`g_va_tracker_skiplist_domain`) pins
// `SkiplistNodeBase` body lifetime; the chunk domain (used only inside
// `resolve_link_target_impl`) pins the underlying `VaChunkDesc`. The
// reservation tables are independent, so cross-domain re-entrancy is
// sound: one chain step pins the chunk descriptor, then the node body
// it resolves to.
//
// Per-thread skiplist-node pin slot assignments (this domain declares
// MaxIdx=4 — the head sentinel is inline in Arena and never retired,
// so no anchor slot is needed):
//
//   0  — walker prev pointer.
//   1  — walker cur pointer; rotated to prev on advance.
//   2  — gather_level_bookmarks prev (top-down GETPREDSUCC walk).
//   3  — gather_level_bookmarks cur.
//
// Slot indices are call-site discipline; the substrate does not enforce
// them. Keeping the mapping fixed makes nested-reader pin lifetimes
// intelligible at a glance.

constexpr uint32_t kPinSlotPrev      = 0;
constexpr uint32_t kPinSlotCur       = 1;
constexpr uint32_t kPinSlotLevelPrev = 2;
constexpr uint32_t kPinSlotLevelCur  = 3;

namespace {

// Context for the multi-step chain decode passed through `protect()`'s
// thunk overload. The thunk reads the packed `Link` word and resolves
// the encoded successor; a foreign helper thread invokes it on the
// helpee's behalf during slow-path helping, with `parent_for_helper`
// kept alive by the PROTECT2 / active-chain CAS scan.
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

// Pinned chain advance. One `protect()` call does the load, era
// convergence and bounded slow-path helping (Nikolaev & Ravindran PLDI
// 2024, §4.2 Fig. 10 generalized form). Lives outside the anonymous
// namespace because `is_walk_range` in the header template calls it
// directly; the body stays here so the chunk-domain plumbing inside
// `resolve_link_target_impl` remains TU-local.
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

// Out-of-line `BatchLinkCodec` definitions. The bodies need access to the
// TU-local `g_bucket_state` and bucket geometry, so they cannot be
// inlined into the header; only the codec declarations are visible at
// the template instantiation site.
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
  // `slot_size` is derived from the encoded bucket — the flat decoder
  // table holds one descriptor per chunk_id, but the slot stride is the
  // bucket's geometry, not anything cached on the descriptor.
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

// `g_va_tracker_arena_domain` lives in arena_alloc.cpp (with `g_arena_state`
// and `BatchLinkCodec<Arena>`). `g_va_tracker_skiplist_chunk_domain` lives
// in va_tracker_chunk.cpp (shared with the ART subsystem's chunks).

// Retire every node the visitor allocated for a Swap that did not
// publish. Records flagged `cleanup_value_on_abort=false` (e.g. the
// caller's pre-allocated RegionDesc passed to is_insert) have their
// payload detached first so the FreeFn does not release caller-owned
// state on a retry.
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

// Commit a fresh skiplist chunk for `bucket_id`, install it under a
// process-global chunk_id, and dual-publish through both the per-bucket
// table and `g_link_chunk_table`. The chunk_id space is shared across
// height buckets (substrate-encoded — `Link::next`'s 8-bit chunk_id has
// process-global meaning), so the installing-sentinel CAS on the global
// table is the single source of truth for "this id is taken".
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
        // Stake out the global chunk_id with the installing sentinel.
        // ACQ_REL on success synchronises with the RELEASE publish below;
        // ACQUIRE on failure synchronises with another thread's publish.
        // The CAS itself is the cross-bucket install lock — exactly one
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

        // Claim the descriptor before committing pages: the descriptor's
        // pool index becomes the pagemap entry's slot_idx, and
        // `commit_chunk` publishes the pagemap entry atomically with the
        // commit transaction.
        VaChunkDesc *cd = va_chunk_desc_pool_claim();
        if (cd == nullptr) {
            g_link_chunk_table[cid].store(nullptr,
                                          cpp::MemoryOrder::RELEASE);
            return nullptr;
        }
        uint32_t pool_idx = va_chunk_pool_index_of(cd);

        // Unified chunk commit: split + commit_replace + partition counter
        // + pagemap register + pagemap publish, with rollback on failure.
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
        // (state=Live, count=0, gen=0) packed into a single 64-bit
        // composite so subsequent count-mutating CAS in
        // `release_va_chunk_slot` is one atomic operation.
        init_live(cd->live_state);
        g_va_tracker_skiplist_chunk_domain.init_node(cd);

        // Replace the installing sentinel with the live cd in one
        // RELEASE store. Readers that pinned the sentinel re-converge
        // via era inside `protect()` and observe the live cd (or fail
        // and restart). Allocator scans iterate the same table.
        g_link_chunk_table[cid].store(cd, cpp::MemoryOrder::RELEASE);
        uint32_t next = (cid + 1) % kSkiplistAddressableChunks;
        b.next_chunk_id_hint.store(next, cpp::MemoryOrder::RELAXED);
        return cd;
    }
    return nullptr;
}

} // namespace

namespace {

// Init context threaded through `va_chunk_acquire_slot` to the per-slot
// initializer. Per-call: identifies the height bucket the slot will join
// and the owning arena to stamp into the node's back-pointer.
struct SkiplistInitCtx {
    uint8_t bucket_id;
    uint8_t height;
    Arena *owning_arena;
    partition_ns::PartitionClass cls;
};

// Per-slot initializer. Runs after `va_chunk_acquire_slot` has zeroed
// the slot and validated chunk/slot canaries. Each `next[i]` is stamped
// LIVE | null with tag 0 — RELAXED is sufficient because the slot is
// not yet visible (publish happens through the caller's CAS into a
// reachable predecessor).
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

// Allocate one fresh skiplist node of the given height. Fast path uses
// the shared `va_chunk_acquire_slot` helper to pick a slot in any live
// chunk; on exhaustion the dual-publish `commit_new_skiplist_chunk_for`
// installs a new chunk and the loop retries. Returns nullptr on
// process-global encoding exhaustion or substrate allocation failure.
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

        // No free slot in any Live chunk. Commit a fresh chunk through
        // the dual-publish install path and re-enter the slot loop.
        VaChunkDesc *cd = commit_new_skiplist_chunk_for(bucket_id, geom);
        if (cd == nullptr)
            return nullptr;
    }
}

// `region_desc_alloc` / `region_desc_release` and
// `clone_region_desc_for_fragment` live in va_region_desc.cpp.
// `arena_alloc` / `arena_retire` / `arena_free`,
// `g_va_tracker_arena_domain` and `BatchLinkCodec<Arena>` live in
// arena_alloc.cpp.

//===----------------------------------------------------------------------===//
//  Harris mark protocol — logical deletion and help-splice
//===----------------------------------------------------------------------===//
//
// Logical deletion is the Harris (PODC 2001) mark bit on the
// predecessor's `next` link. A walker observing MARK on `cur->next[lvl]`
// finalises the splice by CAS-replacing `prev->next[lvl]` with `cur`'s
// successor, then runs `link_finalize_after_splice` to convert `cur`'s
// `mark` to `cert+unmark`. Strong CAS at both sides serialises the
// original marker against any number of concurrent helpers.

namespace {

// Return the level-`level` link of `node`, honouring the inline head
// sentinel's split storage (`head.next[0]` for level 0, the
// `head_tail_links_` array for levels 1..kMaxHeight-1).
[[nodiscard]] LIBC_INLINE cpp::Atomic<Link> &
node_next(Arena *arena, SkiplistNodeBase *node, uint32_t level) {
    return node == &arena->head ? arena->head_next(level) : node->next[level];
}

void help_unlink_marked(SkiplistNodeBase *prev, SkiplistNodeBase *cur,
                        Arena *arena, uint32_t level) {
    cpp::Atomic<Link> &prev_next = node_next(arena, prev, level);
    Link prev_link = prev_next.load(cpp::MemoryOrder::ACQUIRE);
    // Confirm prev still encodes cur as its successor at this level.
    uint16_t cur_enc = encode_link_next(cur->chunk_id, cur->slot_idx);
    if (prev_link.next() != cur_enc)
        return;  // already spliced by someone else

    Link cur_link = cur->next[level].load(cpp::MemoryOrder::ACQUIRE);
    if (!cur_link.is_marked())
        return;  // no longer marked

    // Pivot prev's link past cur. State byte is preserved (prev is on
    // its own state machine); CERT/MARK/ALERT are dropped via
    // `with_next_uncertify` because the new successor inherits a fresh
    // edge. ACQ_REL synchronises the unlink with future walkers.
    uint16_t new_next = cur_link.next();
    Link desired = prev_link.with_next_uncertify(new_next);
    if (!prev_next.compare_exchange_strong(
            prev_link, desired, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
        return;  // CAS race — some other walker will help

    // Convert cur's mark to cert+unmark so it can be retired through
    // Crystalline (the retire predicate refuses still-marked nodes).
    MarkedLinkSnap mks = MarkedLinkSnap::from_observed_marked(cur_link);
    (void)link_finalize_after_splice(cur->next[level], mks);
}

} // namespace

//===----------------------------------------------------------------------===//
//  Find predecessor — level-0 search anchor for Lock and Alloc
//===----------------------------------------------------------------------===//
//
// Walks the skiplist from the head sentinel, descending level by level
// and skipping marked / INVALIDATED nodes (with cooperative help on
// observed MARK). Returns the last node whose `hi <= lo` (the head if
// no such node exists), together with the snapshot of its `next[0]`
// used for downstream CAS sites. `restart` requests a fresh outer
// retry — set on chain-integrity violations or on a marker we cannot
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
// Single top-down pass that captures the predecessor at every level for
// a target interval starting at `lo` (Kim, Kwon & Kang SOSP 2025,
// §A pseudocode lines 4-31). The descent reuses each level's bookmark
// as the starting point for the next level, yielding O(log N + spans)
// per Swap rather than O(log N) per level. Bookmarks are subsequently
// fed into the per-level publish CAS in `publish_new_upper_levels`.
//
// Crystalline pin discipline: only the level-0 prev is pinned at exit
// (slot `kPinSlotLevelPrev`). The upper-level bookmark pointers are
// raw — the caller treats them as optimistic CAS hints. If a bookmarked
// pred is retired and its slot recycled before the caller uses it, the
// publish CAS fails on tag mismatch (substrate T1 tag monotonicity) and
// the caller breaks per the paper's §A line 81. Substrate T3 (never-
// freed pool memory) guarantees the raw deref is at worst a stale read,
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
                // Walker mid-splice on prev — restart the gather; a
                // peer is moving the chain under us.
                out.restart = true;
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
                // Chain integrity: level-i chain holds only height>i.
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
                // cur extends past our target start; prev is the
                // bookmark at this level.
                out.per_level[lvl] = {prev, prev_snap};
                break;
            }

            // Advance: cur becomes the new prev. The O(log N) cost
            // comes from carrying `prev` down across level descents —
            // the next-level walk starts where this level stopped.
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
// Acquires LOCKED on the level-0 predecessor and on every node whose
// interval overlaps `[lo, hi)`, in ascending VA order. The single
// `link_cas_state<LIVE, LOCKED, SkiplistLinkTraits>` per node serves
// triple duty: it linearises the lock acquisition, bumps the link tag
// (closing in-place ABA), and via the traits hook sets
// `LINK_ALERT_FIRED_BIT` so future contention parkers know to recheck.
// No separately-allocated mutex object exists per node — the parking
// word and the lock state share the same atomic.
//
// On observed LOCKED on a successor we release the predecessor LOCK
// before parking, so disjoint VA intervals can make forward progress
// while we sleep (paper §4.4). The successor's release CAS fires the
// alert that wakes us.

bool LockedSet::acquire(Arena *arena_in, uintptr_t lo_in, uintptr_t hi_in) {
    // Defer-acquire pattern: callers carry a default-constructed slot
    // and call `acquire` to populate it in place. A pre-populated slot
    // would leak its existing locks under overwrite, so trap.
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
            // Park directly on the link word. The holder's release-class
            // state CAS will fire `LINK_ALERT_FIRED_BIT` via the traits
            // hook and a paired `futex_addr::wake` will issue
            // `NtAlertThreadByThreadId`.
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

        // Linearisation CAS for the predecessor lock. Tag is bumped and
        // ALERT_FIRED set in the same atomic via `SkiplistLinkTraits`.
        if (!link_cas_snap<SkiplistNodeState::LOCKED, SkiplistLinkTraits>(
                pred->next[0], pred_snap)) {
            continue; // tag mismatch / state changed under us
        }

        // Predecessor locked. Pin it for the walk: although LOCKED makes
        // it structurally retire-blocked, the pin maintains the
        // help-protocol invariants for any peer that observes the link
        // word and tries to help.
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
                // Stash the park target and break to the pre-park
                // release-of-pred path below.
                park_succ = true;
                park_succ_snap = cur_snap;
                park_succ_node = cur;
                break;
            }
            LIBC_ASSERT(cur_state ==
                        static_cast<uint8_t>(SkiplistNodeState::LIVE));

            // Successor lock: same alerting CAS as the predecessor.
            if (!link_cas_snap<SkiplistNodeState::LOCKED, SkiplistLinkTraits>(
                    cur->next[0], cur_snap)) {
                restart = true;
                break;
            }

            if (!set.push(cur)) {
                // Locked-set storage exhausted (overflow block alloc
                // failure). Undo the successful LOCK on cur and surface
                // -ENOMEM. The release CAS fires the alert; the wake
                // call issues NtAlertThreadByThreadId for any parker.
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

            // Advance: cur becomes the next prev. Slot Cur is overwritten
            // on the next `pinned_read_link_target`; slot Prev is
            // re-anchored to stay era-current.
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
            // Pre-park release of every node we have acquired (succs
            // and pred). This is the disjoint-region-parallelism step
            // from paper §4.4 — disjoint mutators must not stack on our
            // holds while we sleep on the contended successor.
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
// Each release CAS is `link_cas_state<LOCKED, LIVE, SkiplistLinkTraits>`,
// so the traits hook flips `LINK_ALERT_FIRED_BIT` atomically with the
// state transition. The explicit `futex_addr::wake` is still required
// because ALERT_FIRED is a hint bit only — the kernel-level wake driver
// is `NtAlertThreadByThreadId`, issued by the wake call itself.

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
// Kim, Kwon & Kang SOSP 2025 §4.3 "collective decrement" (Fig. 8b/c):
// mark every old node's upper-level outgoing link top-down, then attempt
// a per-level batch unlink keyed off the gather-pass bookmarks. On a
// CAS-failure at any level we leave the marks and rely on lazy walkers
// to splice — the level-0 linearisation has already committed, so the
// result is observably correct; missing skip links only degrade
// subsequent searches to slower (but still correct) traversal.

// One eager-unlink attempt at upper level `lvl`. Pivots
// `pred->next[lvl]` past the entire locked run using the bookmark snap;
// on success, finalises every marked old member at this level
// (`mark` -> `cert+unmark`). On CAS failure the marks remain in place
// for lazy help.
[[nodiscard]] bool
try_unlink_old_run_at_level(LockedSet &set, LevelBookmark &bookmark,
                            uint32_t lvl, uint16_t succ_enc) {
    if (bookmark.pred == nullptr)
        return false;

    Arena *arena = set.arena;
    cpp::Atomic<Link> &pred_next = node_next(arena, bookmark.pred, lvl);
    Link expected = bookmark.pred_snap;
    // Pivot next, drop CERT/MARK/ALERT (with_next_uncertify semantics).
    // pred is on-chain and its prior CERT was 0; ALERT is owner-irrelevant
    // at the upper-level non-set predecessor.
    Link desired = expected.with_next_uncertify(succ_enc);
    bool cas_ok = pred_next.compare_exchange_strong(
        expected, desired, cpp::MemoryOrder::ACQ_REL,
        cpp::MemoryOrder::ACQUIRE);
    if (cas_ok) {
        // Refresh bookmark snap to reflect post-unlink state for the
        // upcoming publish CAS at this level.
        bookmark.pred_snap = pred_next.load(cpp::MemoryOrder::ACQUIRE);
        // Finalize marks on every old member at this level.
        finalize_marked_old_level(set, lvl);
        return true;
    }
    // CAS failure — bookmark snap is stale. Refresh from current link
    // so the publish phase has a chance to use the same bookmark.
    bookmark.pred_snap = pred_next.load(cpp::MemoryOrder::ACQUIRE);
    return false;
}

// Set the Harris mark on every old-set member's upper-level outgoing
// link (paper Fig. 8b Phase 1). Runs before the level-0 linearisation —
// Crystalline retirement requires the retired node to be unreachable
// from every upper level, and lazy walkers may not have had time to
// splice on their own.
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
            // No old member has height > lvl — nothing to unlink at
            // this level. Bookmark already reflects the live state.
            continue;
        }
        // succ_enc may be kLinkNullEncoding if every old member's
        // upper-level next is null (last set member at this level had
        // no successor); that's still a valid pivot target.
        (void)try_unlink_old_run_at_level(set, bookmarks.per_level[lvl],
                                           lvl, succ_enc);
    }
}

// Per-level head/tail bookkeeping for the new chain being stitched
// off-list before the linearisation CAS. `left[lvl]` is the leftmost
// new node tall enough to reach level `lvl`; `right[lvl]` is the
// rightmost. Level 0 stays LOCKED until the level-0 publish completes
// so a concurrent overlap update cannot retire one of these nodes
// mid-tower-publish.
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

// Splice the new run into each upper level chain with one CAS per
// level, using the gather-pass bookmark as the expected pred-link snap
// (paper Fig. 18 lines 78-81, Fig. 8e/f). On first failed CAS we
// abandon the remaining levels — paper §A line 81's `break` semantics.
// The level-0 linearisation has already committed by the time this
// runs, so the Swap is correct; abandoning skip levels only degrades
// subsequent traversals until a future op rebalances.
//
// `bookmarks` has been mutated in place by `unlink_upper_levels_*` so
// `bookmark.pred_snap.next()` is the post-old-set successor encoding.
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
            return; // Paper-correct break: missing bookmark, abandon.
        }
        uint16_t left_enc = encode_link_next(left->chunk_id, left->slot_idx);
        // Stitch the new run's tail at this level to the post-set
        // successor (whatever pred currently points at). Plain RELAXED
        // store — runs.right[lvl] is owned exclusively by us until the
        // CAS below makes it observable on the chain.
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
            return; // Paper-correct break.
        }
    }
}

} // namespace

//===----------------------------------------------------------------------===//
//  Swap — paper Algorithm 2 (linearisation point of the mutation)
//===----------------------------------------------------------------------===//
//
// The linearisation point is the single
// `link_cas_snap_relink<LIVE, SkiplistLinkTraits>` on `pred->next[0]`:
// state transitions LOCKED -> LIVE and `next` pivots to the new chain
// head in one atomic. Before that CAS the old set must be unreachable
// from every upper level (Crystalline retirement requires it); after
// it, the new run's upper-level publishes splice in lazily, and the old
// nodes are stamped INVALIDATED and retired through the skiplist
// domain.
//
// The five steps below run in order; a failure in any step before the
// linearisation CAS returns false and Map retries from Lock. The
// upper-level publishes use paper-correct `break` semantics on per-
// level CAS failure (the linearisation is correct regardless; missing
// skip links degrade traversal until a future op rebalances).

bool Swap(LockedSet &set, NewNodes &new_nodes) {
    if (LIBC_UNLIKELY(set.pred == nullptr))
        return false;

    Arena *arena = set.arena;

    // Step 0. Single top-down GETPREDSUCC pass capturing pred bookmarks
    // at every level. The unlink and publish phases reuse the bookmarks
    // as one-shot CAS hints, keeping Swap at O(log N) rather than the
    // O(N log N) of per-level scans.
    LevelBookmarks bookmarks = gather_level_bookmarks(arena, set.lo);
    if (bookmarks.restart) {
        // Concurrent upper-level mutation observed during the gather.
        // Caller's Map retries Lock.
        return false;
    }

    // Step 1. Detach old nodes from every level > 0. Eager-unlink via
    // bookmarks; on per-level CAS failure the marks remain in place and
    // a lazy walker will splice. The level-0 linearisation in step 3
    // requires this step to have observably completed.
    unlink_upper_levels_before_retire(set, bookmarks);

    NewLevelRuns runs;
    if (!new_nodes.empty())
        runs = prepare_new_level_runs(new_nodes);

    // Step 2. Stitch the new chain off-list. Level 0 tail points at the
    // first node beyond the locked range. Upper-level tails are filled
    // immediately before each upper publish CAS, after step 3's
    // linearisation has committed.
    uint16_t tail_succ_enc;
    if (set.count > 0) {
        // The last locked succ's `next[0]` already points past the
        // locked range to the first non-locked node.
        SkiplistNodeBase *last_locked = set.at(set.count - 1);
        Link last_link = last_locked->next[0].load(cpp::MemoryOrder::ACQUIRE);
        tail_succ_enc = last_link.next();
    } else {
        // Pure-insert / no overlap: tail succ is whatever followed pred.
        tail_succ_enc = set.pred_snap.next();
    }

    if (!new_nodes.empty())
        terminate_new_level0_run(runs, tail_succ_enc);

    // Step 3. Linearisation CAS. State transitions LOCKED -> LIVE and
    // `next` pivots to the new head encoding in a single tag bump.
    uint16_t new_head_enc;
    if (!new_nodes.empty()) {
        SkiplistNodeBase *first = runs.left[0];
        new_head_enc = encode_link_next(first->chunk_id, first->slot_idx);
    } else {
        new_head_enc = tail_succ_enc;
    }

    Link pred_now = set.pred->next[0].load(cpp::MemoryOrder::ACQUIRE);
    // Pred state is still LOCKED: Lock established it and nothing in
    // steps 0-2 has released a pred-side LOCK.
    LIBC_ASSERT(pred_now.state() ==
                static_cast<uint8_t>(SkiplistNodeState::LOCKED));

    if (!link_cas_snap_relink<SkiplistNodeState::LIVE, SkiplistLinkTraits>(
            set.pred->next[0], pred_now, new_head_enc)) {
        // Concurrent mutation observed against pred. Caller's Map
        // retries from Lock.
        return false;
    }
    // Release CAS just fired ALERT_FIRED via the traits hook; pair it
    // with the kernel wake target (NtAlertThreadByThreadId).
    ::LIBC_NAMESPACE::futex_addr::wake(
        reinterpret_cast<volatile void *>(&set.pred->next[0].val), 0xFFFFFFFFu);

    // Step 4. Collective multi-level upper publish for new nodes (paper
    // Fig. 8). New level-0 nodes stay LOCKED until this completes,
    // preventing a concurrent overlap update from retiring a node
    // before its tower is fully linked.
    if (!new_nodes.empty()) {
        publish_new_upper_levels(arena, runs, bookmarks);
        release_published_new_nodes(new_nodes);
    }

    // Step 5. Stamp every old node INVALIDATED in forward order. The
    // typed `link_cas_state<LOCKED, INVALIDATED, SkiplistLinkTraits>`
    // is mandatory here: the runtime `link_exchange_state` variant
    // skips the traits `fires_alert` hook and would leave ALERT_FIRED
    // un-set, breaking the contract for any reader still parked on the
    // stale LOCKED snapshot. The explicit `futex_addr::wake` issues
    // `NtAlertThreadByThreadId` — ALERT_FIRED on its own is only a
    // hint bit, not a kernel wake.
    //
    // Every old node is on entry guaranteed LOCKED (Lock established
    // it; nothing between Lock and here releases it). A CAS failure
    // would mean the invariant was violated — trap.
    for (uint32_t i = 0; i < set.count; ++i) {
        SkiplistNodeBase *old_node = set.at(i);
        // Bool temp: `LIBC_UNLIKELY` is single-arg and cannot consume
        // a comma-bearing template-id directly.
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

    // Step 5b. Retire old node descriptors through Crystalline grace.
    // The kernel-state lifecycle for any underlying mapping is closed
    // synchronously by the enclosing Transaction's post-Swap survivor
    // walk (`run_stage2` → `backing_kill_and_retire` on Live → Killed
    // CAS winners), so this retire is metadata-only — the FreeFn
    // touches no kernel handles and makes no `nt_pal::*` call.
    //
    // The locked set is intentionally NOT cleared here: the post-Swap
    // survivor walk relies on walking `set.at(i)` to discover the OLD
    // backings that need teardown. Callers MUST call `Unlock` after
    // they've finished consuming the set (which clears it via
    // `Store::clear`). The Banakar 2025 `Map` template path, which
    // performs no post-Swap walk, clears at the call site immediately
    // after a successful Swap to preserve the destructor's empty-store
    // invariant.
    for (uint32_t i = 0; i < set.count; ++i) {
        g_va_tracker_skiplist_domain.retire(set.at(i));
    }

    return true;
}

//===----------------------------------------------------------------------===//
//  Query — wait-free point lookup
//===----------------------------------------------------------------------===//
//
// Top-down level descent under a Crystalline-W skiplist-domain pin. At
// each level we walk forward while `cur->hi <= key`, then descend; at
// level 0 we walk one more step to find the covering node. Marked nodes
// trigger cooperative help (`help_unlink_marked`); INVALIDATED nodes
// remain readable per Kim, Kwon & Kang §I5 — their `value` field stays
// valid until grace, which is gated by the caller's pin. AS-safe and
// VEH-callable: every internal load goes through the pinned-read helper,
// so a fault on a recycled slot is impossible.

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

    // Per-level step bound is `kTraversalStepLimit = kMaxSkiplistNodes
    // + kMaxHeight`, the structural ceiling on how many distinct nodes
    // can exist under the 8-bit chunk_id × 256 slots = 65535 encoding
    // plus height slack. Exceeding it means chain corruption (cycle or
    // runaway help) and is unreachable under the lock-free invariants
    // — trap rather than spin.
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
            // IDLE is the substrate T2 retire-Retry sentinel; under the
            // pin it is unreachable here. `st` is maybe_unused so
            // release builds don't trip `-Werror,-Wunused-variable`.
            [[maybe_unused]] const uint8_t st = cur_snap.state();
            LIBC_ASSERT(st == static_cast<uint8_t>(SkiplistNodeState::LIVE) ||
                        st == static_cast<uint8_t>(SkiplistNodeState::LOCKED) ||
                        st == static_cast<uint8_t>(SkiplistNodeState::INVALIDATED));
            if (cur->hi <= key) {
                prev = cur;
                g_va_tracker_skiplist_domain.anchor(kPinSlotPrev);
                continue;
            }
            // cur extends past key; descend a level.
            break;
        }
    }
    // Level-0 forward walk: prev is the largest node with `hi <= key`
    // (or the head sentinel). Same `kTraversalStepLimit` bound closes
    // the wait-free invariant on this terminal path.
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
            // Covering node found. INVALIDATED state is acceptable: the
            // node's `value` is held until grace and grace is gated by
            // the caller's pin (Kim, Kwon & Kang §I5).
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
// Find an unoccupied sub-range of length `len` inside `[lo, hi)` and
// insert a node covering it. The per-arena `hint` pointer routes a
// two-pass scan when the hint is usable (probe past the hint first, fall
// back to the full range). Insertion is a single-node Swap: prepare an
// off-list candidate at `pred`'s post-snapshot, CAS-pivot pred to it,
// then run the upper-level publish for the candidate's tower.

namespace {

// Park on a level-0 link word matching `snap` (Lock contention waiter
// shape, factored for Alloc's CAS-fail-retry branches).
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
                        // Linearisation point won. Promote the
                        // candidate's upper levels with a fresh
                        // GETPREDSUCC pass; no old set to unlink.
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
                    // Gap claimed under us or pred-link shifted. Stay on
                    // this predecessor and re-evaluate (paper Alloc
                    // CAS-failure case 3).
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

// xorshift64 (Marsaglia, J. Stat. Soft. 2003) producing a geometric
// distribution over [1, kMaxHeight]. Per-thread TLS state lazily seeded
// from `tid ^ pid ^ partition_secret` defends against correlation across
// concurrent inserters; the all-zero state is reserved as the
// "uninitialised / needs reseed" sentinel and the seeder substitutes a
// golden-ratio fallback if the entropy mix lands on zero.
//
// `sample_height_fork_reseed` (below) clears the surviving thread's
// state after fork so the next call mixes the rotated `partition_secret`
// and the fresh pid; new child threads pick up zero-initialised TLS and
// re-seed naturally on first use.
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
    // Clear TLS state on the surviving thread; the next sample re-seeds
    // against the rotated partition_secret and the fresh pid. The
    // surviving thread's TID is unchanged across fork, so entropy comes
    // entirely from secret + pid.
    g_height_prng_state = 0;
}

// `clone_region_desc_for_fragment` lives in va_region_desc.cpp.

//===----------------------------------------------------------------------===//
//  Insert and erase visitors — driven by Map
//===----------------------------------------------------------------------===//
//
// `is_insert` and `is_erase` are thin wrappers over `Map` that feed it a
// visitor. The visitor receives the locked overlap set and produces a
// `VisitorOutcome` containing the new node list to splice in, the hint
// update, and an error code. Fragment helpers
// (`append_left_fragment` / `append_right_fragment`) preserve the
// surviving prefix/suffix on partial overlap by cloning the old
// RegionDesc and pointing the new node at the shifted backing offset.

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
// Runs asynchronously when the last pin on a retired node drains.
// Validates the node-level canary before any chunk dereference (catches
// use-after-free that survived the substrate's Safety Triad), transitions
// the link word INVALIDATED -> IDLE atomically with CERT (substrate T2
// retire-Retry gate), clears the arena hint if it still points at this
// node, releases the embedded RegionDesc, then drives the chunk
// state-machine teardown. The kernel-state lifecycle for the underlying
// mapping is owned by the enclosing Transaction (closed synchronously
// before commit), so this FreeFn touches no NT handles.

void skiplist_node_free(SkiplistNodeBase *node) {
    if (LIBC_UNLIKELY(node == nullptr))
        __builtin_trap();

    if (LIBC_UNLIKELY(node->bucket == 0xFF))
        __builtin_trap();

    // Validate per-slot canary BEFORE chunk dereference.
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
    // one atomic; any reader still racing through this link observes
    // IDLE and rejects the slot before it can be recycled.
    (void)link_exchange_state_certify(
        node->next[0], static_cast<uint8_t>(SkiplistNodeState::IDLE));

    // Drop the arena hint if it still names this node.
    Arena *arena = node->owning_arena;
    if (arena) {
        SkiplistNodeBase *expected_hint = node;
        arena->hint.compare_exchange_strong(
            expected_hint, nullptr, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE);
    }

    // Detach and release the embedded RegionDesc. The Transaction that
    // commit-retired this node already closed any backing's kernel
    // handles synchronously; this is metadata-only descriptor release.
    RegionDesc *rd = node->value.load(cpp::MemoryOrder::RELAXED);
    if (rd != nullptr) {
        node->value.store(nullptr, cpp::MemoryOrder::RELAXED);
        region_desc_release(rd);
    }

    // Drive the chunk state-machine teardown for this slot. The shared
    // helper handles the flat decoder directly
    release_slot_in_va_chunk(cd, node->slot_idx, g_link_chunk_table);
}

// The shared chunk FreeFn `va_chunk_desc_free` (covering both skiplist
// and ART chunks) lives in `va_tracker_chunk.cpp`.

//===----------------------------------------------------------------------===//
//  Init and fork-reinit
//===----------------------------------------------------------------------===//

void interval_skiplist_init() {
    if (g_init_done.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;

    va_chunk_init();
    arena_init_registration();
    va_chunk_desc_pool_init_once();
    // `g_bucket_state` (rotating hints) and the flat
    // `g_link_chunk_table` are zero-initialised through BSS placement;
    // RegionDesc / Arena per-class state likewise needs no explicit
    // setup here.
}

// Walk a chunk's occupancy bitmap and dispatch to a visitor for every
// set bit. Single-threaded post-fork, so RELAXED bitmap reads suffice.
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

// Per-slot fork visitor for skiplist nodes: refresh the node canary
// against the rotated partition_secret, then release any LOCKED state
// on the level-0 link. LOCKED only ever appears on level 0 (Kim, Kwon
// & Kang §3 — Lock CAS targets `pred->next[0]`); the pre-fork owner of
// the lock is gone, so future acquirers would park forever without
// this scrub.
void fork_scrub_visit_skiplist_node(uint32_t slot, void *slot_ptr,
                                      uint16_t cls_id, uint8_t chunk_id) {
    auto *node = static_cast<SkiplistNodeBase *>(slot_ptr);
    node->node_canary = compute_va_node_canary(
        partition_secret(), cls_id, chunk_id, static_cast<uint8_t>(slot));
    (void)link_cas_state<SkiplistNodeState::LOCKED, SkiplistNodeState::LIVE,
                          SkiplistLinkTraits>(node->next[0]);
}

// RegionDesc per-slot canary refresh lives in va_region_desc.cpp; Arena
// per-slot canary refresh and LOCKED scrub live in arena_alloc.cpp (co-
// located with the Arena slot layout). The master fork hook in this TU
// contributes the `mark_reachable` visitor used by the leaked-descriptor
// reclaim pass.

//===----------------------------------------------------------------------===//
//  Fork leaked-descriptor reclaim
//===----------------------------------------------------------------------===//
//
// Pre-fork, dead threads' Crystalline cells held retire batches whose
// FreeFn would return the `VaChunkDesc` slot to the shared pool and
// decommit chunk pages. In the child those threads are gone, so the
// FreeFn never runs — the descriptor's pool slot leaks and the chunk
// pages stay committed forever.
//
// Crystalline stays fork-unaware. The reclaim instead walks every
// owned chunk_table to compute the reachable-descriptor set, then walks
// the shared chunk-desc pool and reclaims any claimed descriptor whose
// bucket belongs to this subsystem but is not reachable. Bounded by the
// pre-fork in-flight retire cardinality and safe because the reachable
// set is built from chunk_table snapshots at fork, when no peer thread
// can republish.

struct ForkReclaimCtx {
    // One bit per pool slot. Set bit = "reachable from one of our
    // chunk_tables, do not reclaim."
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

// `void(VaChunkDesc *, void *)` adapter over `mark_reachable`, consumed
// by `arena_fork_reinit_phase` and any other subsystem's chunk-visit
// surface. The opaque `void *` is the reclaim context pointer.
void mark_reachable_visit(VaChunkDesc *cd, void *ctx_p) {
    auto *ctx = static_cast<ForkReclaimCtx *>(ctx_p);
    mark_reachable(*ctx, cd);
}

// Pool-walk visitor: reclaim leaked descriptors for the buckets this
// subsystem owns (skiplist height buckets 0..3, RegionDesc, Arena).
// DescBacking (bucket 6) is reclaimed in `desc_backing.cpp`, and ART
// buckets (7..10) by the ART subsystem's own fork hook; each owner
// reclaims only its own buckets so a single pool walk does not
// duplicate work.
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
        return;  // Reachable — leave alone.

    // Leaked. Decommit chunk pages and return the descriptor slot.
    if (cd->partition != nullptr && cd->chunk_base != nullptr) {
        partition_ns::decommit_chunk(cd->partition, cd->chunk_base,
                                     cd->chunk_bytes);
    }
    mark_dead(cd->live_state);
    va_chunk_desc_pool_release(cd);
}

} // namespace

void interval_skiplist_fork_reinit() {
    // Drain pending retires on the surviving thread. The Arena
    // domain's `clear_all` runs inside `arena_fork_reinit_phase` below.
    g_va_tracker_skiplist_domain.clear_all();
    va_chunk_fork_reinit();

    // Clear the height-PRNG TLS state. Must run after
    // `va_chunk_fork_reinit` so the next sample mixes the rotated
    // partition_secret rather than the pre-fork one.
    sample_height_fork_reseed();

    // Per chunk_table, two passes per chunk:
    //   1. Refresh `chunk_canary` against the rotated partition_secret.
    //   2. Walk the occupancy bitmap; for each live slot refresh the
    //      per-slot canary and scrub any stale LOCKED state.
    //
    // The same walk marks each reachable descriptor in
    // `reclaim_ctx.reachable_bits`, feeding the leaked-descriptor
    // reclaim below. Skipping the per-slot canary pass would leave the
    // next FreeFn observing `node_canary != compute_va_node_canary
    // (new_secret, ...)` and trapping.
    // One pass over the flat decoder covers every height bucket — each
    // descriptor's `bucket_id` resolves its class_id for canary
    // derivation. Skips the in-progress install sentinel (numerically a
    // low-address non-pointer; never a valid descriptor).
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
    // RegionDesc and Arena fork-reinit live in their own TUs; both
    // contribute reachable descriptors to the master reclaim context
    // through `mark_reachable_visit`.
    region_desc_fork_reinit_phase(&mark_reachable_visit, &reclaim_ctx);
    arena_fork_reinit_phase(&mark_reachable_visit, &reclaim_ctx);

    // Reclaim every claimed descriptor that no chunk_table references.
    // These are the slots dead-thread Crystalline cells were holding
    // through pre-fork retire batches whose FreeFn will never run.
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
    // Single pass over the flat decoder, tally by `cd->bucket_id`. Skips
    // null and the in-progress install sentinel.
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
