//===- va_tracker_chunk.cpp - Shared per-region chunk allocator (impl) ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the va_tracker chunk allocator used by both the interval
// skiplist (Kim/Kwon/Kang, SOSP 2025) leaf side and the ROWEX ART (Leis 2016)
// outer index. Hosts the process-lifetime `VaChunkDesc` pool, the chunk
// commit driver, the drain orchestrator, and the two Crystalline-W chunk
// domains.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/alloc/partition_numa_select.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

namespace partition_ns = alloc::partition;

//===----------------------------------------------------------------------===//
//  Crystalline-W chunk domain instances
//===----------------------------------------------------------------------===//

// The two domains share the same FreeFn but distinct grace machinery so the
// skiplist's chunk-table reader-race window is independent of the ART's.

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx>
    g_va_tracker_skiplist_chunk_domain;

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx>
    g_va_tracker_art_chunk_domain;

//===----------------------------------------------------------------------===//
//  VaChunkDesc pool
//===----------------------------------------------------------------------===//

// Process-lifetime BSS pool. Each bucket reserves `kChunksPerPoolBucket` =
// 256 entries; the 11-bucket pool is 2816 entries x 128 B = 352 KiB total,
// demand-faulted from BSS so idle cost is approximately zero.

namespace {

// The value-init brace pair forces zero-init of every cpp::Atomic member.
// Without it the defaulted ctor leaves the atomic values indeterminate and
// `-Werror=-Wglobal-constructors` fires on the pool. Same pattern as
// `ArenaState` in `buddy_arena.cpp`.
VaChunkDesc g_va_chunk_desc_pool[kTotalVaChunkDescPoolSize]{};

constexpr uint32_t kVaChunkDescPoolWords =
    (kTotalVaChunkDescPoolSize + 63) / 64;
cpp::Atomic<uint64_t> g_va_chunk_desc_pool_free[kVaChunkDescPoolWords]{};

cpp::Atomic<uint32_t> g_va_chunk_desc_pool_inited{0};

// Caches the per-process partition secret read out of PCB Zone 0b. Rotated
// on fork by the PCB itself; subsystem-side canaries refresh by calling
// `compute_va_chunk_canary` again with the fresh secret.
LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

} // anonymous namespace

void va_chunk_desc_pool_init_once() {
    // Single-shot latch: only the thread that flips 0 -> 1 runs the
    // initializer; every other caller observes the bitmap already set.
    if (g_va_chunk_desc_pool_inited.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;
    for (uint32_t w = 0; w < kVaChunkDescPoolWords; ++w) {
        uint64_t v = ~uint64_t{0};
        // Trailing word may cover fewer than 64 entries; mask to the valid
        // range so phantom bits past the pool capacity stay zero (i.e. "in
        // use", unclaimable).
        if (w == kVaChunkDescPoolWords - 1) {
            uint32_t tail = kTotalVaChunkDescPoolSize -
                            (kVaChunkDescPoolWords - 1) * 64;
            if (tail < 64)
                v = (uint64_t{1} << tail) - 1;
        }
        g_va_chunk_desc_pool_free[w].store(v, cpp::MemoryOrder::RELAXED);
    }
}

VaChunkDesc *va_chunk_desc_pool_base() {
    return g_va_chunk_desc_pool;
}

size_t va_chunk_desc_pool_capacity() {
    return kTotalVaChunkDescPoolSize;
}

VaChunkDesc *va_chunk_desc_pool_claim() {
    va_chunk_desc_pool_init_once();
    for (uint32_t w = 0; w < kVaChunkDescPoolWords; ++w) {
        for (;;) {
            uint64_t old = g_va_chunk_desc_pool_free[w].load(
                cpp::MemoryOrder::ACQUIRE);
            if (old == 0)
                break;
            // `old & -old` isolates the lowest set bit (a classic two's-
            // complement trick); the CAS clears that bit, claiming exactly
            // one free slot per successful iteration.
            uint64_t bit = old & -old;
            uint64_t desired = old ^ bit;
            if (g_va_chunk_desc_pool_free[w].compare_exchange_weak(
                    old, desired, cpp::MemoryOrder::ACQ_REL,
                    cpp::MemoryOrder::ACQUIRE)) {
                int idx_in_word = __builtin_ctzll(bit);
                uint32_t idx = w * 64 + idx_in_word;
                if (idx >= kTotalVaChunkDescPoolSize)
                    break;
                VaChunkDesc *cd = &g_va_chunk_desc_pool[idx];
                __builtin_memset(static_cast<void *>(cd), 0,
                                 sizeof(VaChunkDesc));
                return cd;
            }
        }
    }
    return nullptr;
}

void va_chunk_desc_pool_release(VaChunkDesc *cd) {
    LIBC_ASSERT(cd >= g_va_chunk_desc_pool &&
                cd < g_va_chunk_desc_pool + kTotalVaChunkDescPoolSize);
    uint32_t idx = static_cast<uint32_t>(cd - g_va_chunk_desc_pool);
    uint32_t w = idx / 64;
    uint64_t bit = uint64_t{1} << (idx % 64);
    g_va_chunk_desc_pool_free[w].fetch_or(bit, cpp::MemoryOrder::ACQ_REL);
}

//===----------------------------------------------------------------------===//
//  Commit driver
//===----------------------------------------------------------------------===//

// Body of `commit_new_va_chunk_for` — see header for parameter / return
// semantics. The function performs partition selection, descriptor claim,
// unified chunk commit, descriptor population, Crystalline node init, and
// CAS-installation into the chunk table, with rollback on race-loss.

VaChunkDesc *commit_new_va_chunk_for(
    partition_ns::PartitionClass cls,
    cpp::Atomic<VaChunkDesc *> *chunk_table,
    cpp::Atomic<uint32_t> *next_chunk_id_hint,
    uint8_t bucket_id, uint32_t slot_size, uint32_t slots_per_chunk,
    uint32_t chunk_bytes) {
    // The NUMA selector returns `kNodeAgnostic` for every class except the
    // user-facing `Alloc{Small,Medium,Large,Huge}` band — including every
    // va_tracker class this function is currently invoked with. The call
    // shape is forward-compatible: when a future caller passes a
    // user-replicable class, the selector resolves to the calling thread's
    // preferred NUMA node (single-node systems still short-circuit to
    // `kNodeAgnostic`).
    uint16_t target_node = partition_ns::pick_node_for_alloc(cls);
    partition_ns::PartitionDescriptor *part =
        partition_ns::reserve_or_grow(cls, target_node);
    if (LIBC_UNLIKELY(part == nullptr))
        return nullptr;

    uint32_t hint = next_chunk_id_hint->load(cpp::MemoryOrder::ACQUIRE);
    for (uint32_t attempt = 0; attempt < kChunksPerPoolBucket; ++attempt) {
        uint32_t cid = (hint + attempt) % kChunksPerPoolBucket;
        VaChunkDesc *expected = nullptr;
        if (chunk_table[cid].load(cpp::MemoryOrder::ACQUIRE) != nullptr)
            continue;

        uintptr_t base = reinterpret_cast<uintptr_t>(part->base);
        uintptr_t chunk_va =
            base + partition_ns::kPartitionGuardBytes +
            static_cast<uintptr_t>(cid) * static_cast<uintptr_t>(chunk_bytes);
        void *chunk_base = reinterpret_cast<void *>(chunk_va);

        // Claim the descriptor BEFORE committing pages: the descriptor's
        // pool index is the pagemap entry's `slot_idx`, and `commit_chunk`
        // publishes the pagemap entry as part of the unified commit
        // transaction.
        VaChunkDesc *cd = va_chunk_desc_pool_claim();
        if (cd == nullptr)
            return nullptr;
        uint32_t pool_idx = va_chunk_pool_index_of(cd);

        // Unified chunk commit: split placeholder + commit_replace +
        // partition counter + pagemap register + pagemap publish, with
        // rollback on each failure.
        int rc = partition_ns::commit_chunk(
            part, chunk_base, chunk_bytes, PAGE_READWRITE,
            ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer::VaTrackerVaChunk,
            pool_idx);
        if (rc != 0) {
            va_chunk_desc_pool_release(cd);
            // -EIO from a placeholder split conflict means another thread
            // beat us to this cid; -EAGAIN from partition retire-state
            // means the caller should retry via reserve_or_grow. Both
            // fall through to "try the next cid" via continue.
            if (rc == -EAGAIN)
                return nullptr;
            continue;
        }

        cd->chunk_base = chunk_base;
        cd->slot_size = slot_size;
        cd->slot_capacity = slots_per_chunk;
        cd->chunk_bytes = chunk_bytes;
        cd->bucket_id = bucket_id;
        cd->chunk_id = static_cast<uint8_t>(cid);
        cd->partition = part;
        cd->chunk_canary = compute_va_chunk_canary(
            partition_secret(), static_cast<uint16_t>(cls),
            static_cast<uint8_t>(cid));
        cd->occupancy.clear_all();
        // Composite live_state init: (state=LIVE, count=0, gen=0).
        init_live(cd->live_state);

        // Init the Crystalline node header on the descriptor in whichever
        // domain it will retire through. Both domains use the same FreeFn,
        // and `CrystallineDomain::init_node` only stamps birth_era +
        // batch_link, which is identical across our two domains. The
        // bucket-to-domain split is centralised in `pick_chunk_domain`.
        pick_chunk_domain(bucket_id).init_node(cd);

        if (chunk_table[cid].compare_exchange_strong(
                expected, cd, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::ACQUIRE)) {
            // CAS-installation is the linearization point at which the
            // chunk becomes visible to reader paths. Advance the rotating
            // hint past this cid so the next commit-side caller starts
            // looking at the next slot.
            next_chunk_id_hint->store((cid + 1) % kChunksPerPoolBucket,
                                       cpp::MemoryOrder::RELAXED);
            return cd;
        }

        // Race-loss: another thread published at this cid first. Roll
        // back our commit (pages + counter + pagemap entry) and release
        // the descriptor.
        partition_ns::decommit_chunk(part, chunk_base, chunk_bytes);
        va_chunk_desc_pool_release(cd);
        return chunk_table[cid].load(cpp::MemoryOrder::ACQUIRE);
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
//  Drain orchestrator
//===----------------------------------------------------------------------===//

// Bitmap clear (mark_dead) before count decrement (release_va_chunk_slot)
// is load-bearing: the just-released bit must be visible to peer allocators
// that race in *before* the count decrement settles. If we decremented
// first, a peer that snapped the lower count and ran `try_va_chunk_reserve`
// could find the bitmap still full and bounce off.
void release_slot_in_va_chunk(VaChunkDesc *cd, uint32_t slot_idx,
                               cpp::Atomic<VaChunkDesc *> *chunk_table) {
    cd->occupancy.mark_dead(slot_idx);
    if (!release_va_chunk_slot(cd->live_state)) {
        // Either count > 0 after decrement (peer still holding a slot or
        // a reservation), or state was already RETIRING / RETIRED (peer
        // drain in flight). Caller does nothing.
        return;
    }

    // We are the unique drain winner. Clear the chunk_table entry before
    // retire so any new reader observes nullptr immediately; readers that
    // already pinned `cd` via the chunk domain's `read()` keep it valid
    // through the grace window.
    chunk_table[cd->chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);

    // Chunk pages stay committed until the descriptor's batch passes
    // grace and `va_chunk_desc_free` runs the decommit; any pinned
    // reader's slot dereferences still land on live pages.
    pick_chunk_domain(cd->bucket_id).retire(cd);
}

//===----------------------------------------------------------------------===//
//  Shared chunk-bitmap allocator scaffold
//===----------------------------------------------------------------------===//

// Single-pass acquire over `spec.chunk_table` using the rotating-hint
// pattern originally shipped only with the skiplist's `bucket_alloc_node`.
// The hint is RELAXED — a load-spreading suggestion, not a publish; the
// reservation CAS on `cd->live_state` is the real linearisation point for
// slot ownership.

void *va_chunk_acquire_slot(const VaChunkAcquireSpec &spec) {
    uint32_t hint =
        spec.next_chunk_id_hint->load(cpp::MemoryOrder::ACQUIRE) %
        spec.chunk_count;
    uint64_t secret = partition_secret();
    auto &domain = pick_chunk_domain(spec.consumer_bucket_id);
    for (uint32_t scan = 0; scan < spec.chunk_count; ++scan) {
        uint32_t cid = (hint + scan) % spec.chunk_count;
        // Pin via domain.protect so `cd` is safe to dereference through
        // the rest of this iteration. Era convergence closes the
        // load <-> refresh race that a manual load would leave open.
        // The domain is picked from the consumer's bucket so ART
        // consumers (buckets 7..10) pin and retire through
        // `g_va_tracker_art_chunk_domain` rather than the skiplist's.
        VaChunkDesc *cd = domain.protect(
            spec.chunk_table[cid], kVaChunkPinSlot, /*parent=*/nullptr);
        if (cd == nullptr)
            continue;
        // Skip in-progress install sentinels — another installer holds
        // this cid; the descriptor pointer is not yet meaningful.
        if (cd == va_chunk_installing_sentinel())
            continue;
        // Reject foreign-bucket chunks before paying any CAS cost. Load-
        // bearing for the skiplist's flat table shared across four
        // height buckets; trivially passes for homogeneous tables
        // (RegionDesc / Arena / DescBacking / ART-per-type) where every
        // chunk carries the consumer's own bucket id.
        if (cd->bucket_id != spec.consumer_bucket_id)
            continue;

        // `try_va_chunk_reserve` atomically establishes (state == LIVE
        // && count < cap) and increments count. The reservation IS the
        // count bump — holding it prevents concurrent drains.
        if (!try_va_chunk_reserve(cd->live_state, spec.slots_per_chunk))
            continue;

        uint32_t slot = try_acquire_first_free_slot(cd, spec.slots_per_chunk);
        if (slot >= spec.slots_per_chunk) {
            // Bitmap raced out — release the phantom reservation. May
            // trigger drain if we were the last reserver of an
            // otherwise-empty chunk; mirror the chunk_table clear and
            // Crystalline retire so the failure path stays consistent
            // with the FreeFn releaser branch.
            if (release_va_chunk_slot(cd->live_state)) {
                spec.chunk_table[cid].store(nullptr,
                                            cpp::MemoryOrder::RELEASE);
                domain.retire(cd);
            }
            continue;
        }

        validate_chunk_or_trap(cd, spec.cls, /*expected_cid=*/cid, secret);
        validate_slot_or_trap(cd, slot);

        uintptr_t base = reinterpret_cast<uintptr_t>(cd->chunk_base);
        void *slot_ptr = reinterpret_cast<void *>(
            base + static_cast<size_t>(slot) *
                       static_cast<size_t>(cd->slot_size));
        __builtin_memset(slot_ptr, 0,
                         static_cast<size_t>(cd->slot_size));

        spec.init(slot_ptr, cd, cid, slot, spec.init_ctx);
        return slot_ptr;
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
//  va_chunk_desc_free — Crystalline FreeFn for VaChunkDesc
//===----------------------------------------------------------------------===//

// Runs after the chunk-domain batch this descriptor was retired in passes
// Crystalline-W grace (every reservation cell that attached to the batch
// has drained). At this point no live reader can hold a pinned `cd`
// pointer or be dereferencing the chunk pages.
//
// Steps:
//   1. Validate per-chunk canary (catches heap-spray of a recycled pool
//      slot before any dereference of `desc->partition` / `chunk_base`).
//   2. Decommit chunk pages back to the partition. Decommit runs here
//      rather than at release time so chunk page lifetime tracks chunk-
//      domain grace; otherwise a Crystalline-pinned reader could observe
//      a valid `cd` but decommitted slot pages.
//   3. Mark the chunk-state-machine state RETIRED (tombstone).
//   4. Return the descriptor's pool slot for fresh-chunk reuse.
//
// Same body for both chunk domains (`g_va_tracker_skiplist_chunk_domain`
// and `g_va_tracker_art_chunk_domain`); dispatch on `bucket_id` only for
// canary derivation. `desc` holds `partition`, `chunk_base`,
// `slot_size`, `slot_capacity` — everything needed to reconstruct the
// decommit args without consulting the chunk_table (which is already
// cleared by `release_slot_in_va_chunk`).
void va_chunk_desc_free(VaChunkDesc *desc) {
    if (LIBC_UNLIKELY(desc == nullptr))
        __builtin_trap();

    uint16_t class_id =
        static_cast<uint16_t>(va_class_for_pool_bucket(desc->bucket_id));
    uint64_t expected =
        compute_va_chunk_canary(partition_secret(), class_id, desc->chunk_id);
    if (LIBC_UNLIKELY(desc->chunk_canary != expected))
        __builtin_trap();

    if (desc->partition != nullptr && desc->chunk_base != nullptr) {
        // Use the chunk's *registered* size — pagemap-aligned chunk_bytes
        // can exceed `slot_size * slot_capacity` (e.g. a 64 KiB chunk
        // with 256 x 64 B = 16 KiB of slots plus 48 KiB of pagemap-
        // aligned slack). Commit and decommit must match the size used
        // at `commit_chunk` time, which is stored on the descriptor.
        partition_ns::decommit_chunk(desc->partition, desc->chunk_base,
                                      desc->chunk_bytes);
    }

    mark_dead(desc->live_state);
    va_chunk_desc_pool_release(desc);
}

//===----------------------------------------------------------------------===//
//  Init and fork
//===----------------------------------------------------------------------===//

namespace {
cpp::Atomic<uint32_t> g_va_chunk_inited{0};
} // anonymous namespace

void va_chunk_init() {
    if (g_va_chunk_inited.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;
    g_va_tracker_skiplist_chunk_domain.init_registration();
    g_va_tracker_art_chunk_domain.init_registration();
    va_chunk_desc_pool_init_once();
}

void va_chunk_fork_reinit() {
    // Drop every retire batch inherited from the parent. Per-bucket
    // canary refresh is the responsibility of each consumer (e.g.
    // `interval_skiplist_fork_reinit` walks the bucket chunk_tables).
    g_va_tracker_skiplist_chunk_domain.clear_all();
    g_va_tracker_art_chunk_domain.clear_all();
}

//===----------------------------------------------------------------------===//
//  Pool iteration and diagnostics
//===----------------------------------------------------------------------===//

void for_each_claimed_va_chunk_desc(VaChunkDescVisitFn visit, void *ctx) {
    for (uint32_t w = 0; w < kVaChunkDescPoolWords; ++w) {
        // Free bits are 1; we want claimed bits, so invert.
        uint64_t claimed = ~g_va_chunk_desc_pool_free[w].load(
            cpp::MemoryOrder::RELAXED);
        // Mask the trailing word to its valid range so an out-of-range
        // bit does not get dispatched.
        if (w == kVaChunkDescPoolWords - 1) {
            uint32_t tail = kTotalVaChunkDescPoolSize -
                            (kVaChunkDescPoolWords - 1) * 64;
            if (tail < 64)
                claimed &= (uint64_t{1} << tail) - 1;
        }
        while (claimed != 0) {
            uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(claimed));
            // Clear the lowest set bit (`x & (x - 1)`) to iterate.
            claimed &= claimed - 1;
            uint32_t idx = w * 64 + bit;
            visit(&g_va_chunk_desc_pool[idx], ctx);
        }
    }
}

uint32_t va_chunk_pool_index_of(VaChunkDesc *cd) {
    if (LIBC_UNLIKELY(cd < g_va_chunk_desc_pool ||
                       cd >= g_va_chunk_desc_pool + kTotalVaChunkDescPoolSize))
        __builtin_trap();
    return static_cast<uint32_t>(cd - g_va_chunk_desc_pool);
}

uint32_t total_live_va_chunks() {
    uint32_t live = 0;
    for (uint32_t w = 0; w < kVaChunkDescPoolWords; ++w) {
        uint64_t v = g_va_chunk_desc_pool_free[w].load(cpp::MemoryOrder::RELAXED);
        // Free bits are 1; in-use bits are 0. Count the in-use bits within
        // the valid range of the word.
        uint64_t cap_bits =
            (w == kVaChunkDescPoolWords - 1)
                ? (kTotalVaChunkDescPoolSize - (kVaChunkDescPoolWords - 1) * 64)
                : 64;
        if (cap_bits >= 64)
            cap_bits = 64;
        uint64_t mask = (cap_bits == 64) ? ~uint64_t{0}
                                          : ((uint64_t{1} << cap_bits) - 1);
        live += __builtin_popcountll(mask & ~v);
    }
    return live;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
