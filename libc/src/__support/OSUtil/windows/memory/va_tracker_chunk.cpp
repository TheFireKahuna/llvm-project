//===- va_tracker_chunk.cpp - Shared per-region chunk allocator (impl) ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx>
    g_va_tracker_skiplist_chunk_domain;

::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    VaChunkDesc, &va_chunk_desc_free, kVaChunkRetireFreq, kVaChunkMaxIdx>
    g_va_tracker_art_chunk_domain;

//===----------------------------------------------------------------------===//
//  VaChunkDesc pool
//===----------------------------------------------------------------------===//

namespace {

// Value-init braces force zero-init of every cpp::Atomic member;
// without them -Werror=-Wglobal-constructors fires on the pool.
VaChunkDesc g_va_chunk_desc_pool[kTotalVaChunkDescPoolSize]{};

constexpr uint32_t kVaChunkDescPoolWords =
    (kTotalVaChunkDescPoolSize + 63) / 64;
cpp::Atomic<uint64_t> g_va_chunk_desc_pool_free[kVaChunkDescPoolWords]{};

cpp::Atomic<uint32_t> g_va_chunk_desc_pool_inited{0};

LIBC_INLINE uint64_t partition_secret() {
    return ::LIBC_NAMESPACE::g_pcb.zone0b.partition_secret();
}

} // anonymous namespace

void va_chunk_desc_pool_init_once() {
    if (g_va_chunk_desc_pool_inited.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return;
    for (uint32_t w = 0; w < kVaChunkDescPoolWords; ++w) {
        uint64_t v = ~uint64_t{0};
        // Tail-mask: phantom bits past pool capacity stay 0 ("in use",
        // unclaimable) so claim never returns an out-of-range index.
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
            // `old & -old` isolates the lowest set bit; CAS clears it,
            // claiming exactly one slot per successful iteration.
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

VaChunkDesc *commit_new_va_chunk_for(
    partition_ns::PartitionClass cls,
    cpp::Atomic<VaChunkDesc *> *chunk_table,
    cpp::Atomic<uint32_t> *next_chunk_id_hint,
    uint8_t bucket_id, uint32_t slot_size, uint32_t slots_per_chunk,
    uint32_t chunk_bytes) {
    // Every va_tracker class currently resolves to kNodeAgnostic; the
    // call is forward-compatible with future user-replicable classes.
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

        // Claim BEFORE committing: the pool index is the pagemap
        // entry's slot_idx, and commit_chunk publishes the pagemap entry
        // as part of the unified commit transaction.
        VaChunkDesc *cd = va_chunk_desc_pool_claim();
        if (cd == nullptr)
            return nullptr;
        uint32_t pool_idx = va_chunk_pool_index_of(cd);

        int rc = partition_ns::commit_chunk(
            part, chunk_base, chunk_bytes, PAGE_READWRITE,
            ::LIBC_NAMESPACE::windows::alloc::VaChunkConsumer::VaTrackerVaChunk,
            pool_idx);
        if (rc != 0) {
            va_chunk_desc_pool_release(cd);
            // -EAGAIN: partition is mid-retire; the descriptor is unusable,
            // caller must restart via `reserve_or_grow`. Any other rc (today
            // only -EIO from a split conflict or `commit_replace` syscall
            // failure) leaves partition counters consistent — just advance
            // to the next cid and retry the install.
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
        init_live(cd->live_state);

        // init_node stamps birth_era + batch_link only; both domains
        // produce identical headers, but the descriptor must retire
        // through the same domain it was init'd in.
        pick_chunk_domain(bucket_id).init_node(cd);

        if (chunk_table[cid].compare_exchange_strong(
                expected, cd, cpp::MemoryOrder::ACQ_REL,
                cpp::MemoryOrder::ACQUIRE)) {
            // CAS-install is the chunk's reader-visibility
            // linearisation point.
            next_chunk_id_hint->store((cid + 1) % kChunksPerPoolBucket,
                                       cpp::MemoryOrder::RELAXED);
            return cd;
        }

        // Race-loss: roll back our commit (pages + counter + pagemap)
        // and hand back the peer's descriptor for the outer alloc loop.
        partition_ns::decommit_chunk(part, chunk_base, chunk_bytes);
        va_chunk_desc_pool_release(cd);
        return chunk_table[cid].load(cpp::MemoryOrder::ACQUIRE);
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
//  Drain orchestrator
//===----------------------------------------------------------------------===//

void release_slot_in_va_chunk(VaChunkDesc *cd, uint32_t slot_idx,
                               cpp::Atomic<VaChunkDesc *> *chunk_table) {
    // Bitmap clear MUST precede the count decrement: a peer that
    // observes the lower count and runs try_va_chunk_reserve must see
    // the freed bit, otherwise it bounces off a "full" bitmap.
    cd->occupancy.mark_dead(slot_idx);
    if (!release_va_chunk_slot(cd->live_state))
        return;

    // Drain winner. Clear the table entry BEFORE retire so any new
    // reader observes nullptr immediately; already-pinned readers keep
    // `cd` valid through the grace window.
    chunk_table[cd->chunk_id].store(nullptr, cpp::MemoryOrder::RELEASE);
    pick_chunk_domain(cd->bucket_id).retire(cd);
}

//===----------------------------------------------------------------------===//
//  Shared chunk-bitmap allocator scaffold
//===----------------------------------------------------------------------===//

void *va_chunk_acquire_slot(const VaChunkAcquireSpec &spec) {
    uint32_t hint =
        spec.next_chunk_id_hint->load(cpp::MemoryOrder::ACQUIRE) %
        spec.chunk_count;
    uint64_t secret = partition_secret();
    auto &domain = pick_chunk_domain(spec.consumer_bucket_id);
    for (uint32_t scan = 0; scan < spec.chunk_count; ++scan) {
        uint32_t cid = (hint + scan) % spec.chunk_count;
        // protect()'s era convergence closes the load<->refresh race
        // that a manual ACQUIRE load would leave open.
        VaChunkDesc *cd = domain.protect(
            spec.chunk_table[cid], kVaChunkPinSlot, /*parent=*/nullptr);
        if (cd == nullptr)
            continue;
        if (cd == va_chunk_installing_sentinel())
            continue;
        // Bucket filter is load-bearing for the skiplist's flat table
        // shared across four height buckets.
        if (cd->bucket_id != spec.consumer_bucket_id)
            continue;

        if (!try_va_chunk_reserve(cd->live_state, spec.slots_per_chunk))
            continue;

        uint32_t slot = try_acquire_first_free_slot(cd, spec.slots_per_chunk);
        if (slot >= spec.slots_per_chunk) {
            // Bitmap raced out. Rollback may make us the drain winner of
            // an otherwise-empty chunk; mirror the FreeFn releaser branch
            // so the failure path stays consistent with the regular drain.
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

// Decommit is deferred from release_slot_in_va_chunk to here so chunk
// page lifetime tracks chunk-domain grace — otherwise a pinned reader
// could observe a valid `cd` but decommitted slot pages.
void va_chunk_desc_free(VaChunkDesc *desc) {
    if (LIBC_UNLIKELY(desc == nullptr))
        __builtin_trap();

    // Canary check BEFORE any dereference of `partition`/`chunk_base` —
    // catches heap-spray of a recycled pool slot.
    uint16_t class_id =
        static_cast<uint16_t>(va_class_for_pool_bucket(desc->bucket_id));
    uint64_t expected =
        compute_va_chunk_canary(partition_secret(), class_id, desc->chunk_id);
    if (LIBC_UNLIKELY(desc->chunk_canary != expected))
        __builtin_trap();

    if (desc->partition != nullptr && desc->chunk_base != nullptr) {
        // chunk_bytes (not slot_size * slot_capacity) so decommit matches
        // the commit-time pagemap-aligned size; slot byte total may be
        // smaller when slots do not pack cleanly into 64 KiB.
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
    // Drop inherited retire batches; per-bucket canary refresh is each
    // consumer's own reinit hook's job.
    g_va_tracker_skiplist_chunk_domain.clear_all();
    g_va_tracker_art_chunk_domain.clear_all();
}

//===----------------------------------------------------------------------===//
//  Pool iteration and diagnostics
//===----------------------------------------------------------------------===//

void for_each_claimed_va_chunk_desc(VaChunkDescVisitFn visit, void *ctx) {
    for (uint32_t w = 0; w < kVaChunkDescPoolWords; ++w) {
        // Freelist convention is free=1; invert to walk claimed slots.
        uint64_t claimed = ~g_va_chunk_desc_pool_free[w].load(
            cpp::MemoryOrder::RELAXED);
        if (w == kVaChunkDescPoolWords - 1) {
            uint32_t tail = kTotalVaChunkDescPoolSize -
                            (kVaChunkDescPoolWords - 1) * 64;
            if (tail < 64)
                claimed &= (uint64_t{1} << tail) - 1;
        }
        while (claimed != 0) {
            uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(claimed));
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
        uint64_t cap_bits =
            (w == kVaChunkDescPoolWords - 1)
                ? (kTotalVaChunkDescPoolSize - (kVaChunkDescPoolWords - 1) * 64)
                : 64;
        if (cap_bits >= 64)
            cap_bits = 64;
        uint64_t mask = (cap_bits == 64) ? ~uint64_t{0}
                                          : ((uint64_t{1} << cap_bits) - 1);
        // Free=1; in-use=0 within the tail-masked range.
        live += __builtin_popcountll(mask & ~v);
    }
    return live;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
