//===- va_tracker.cpp - VA tracker read-side, fork, bootstrap -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Read-side machinery, arena resolve, fork hooks, bootstrap. The mutating
// surface lives in va_tracker_transaction.cpp (frame + public ops) and
// va_tracker_execute.cpp (per-op kernel programs + reaper).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/va_tracker.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/art_index.h"
#include "src/__support/OSUtil/windows/memory/art_node.h"
#include "src/__support/OSUtil/windows/memory/art_node_alloc.h"
#include "src/__support/OSUtil/windows/memory/desc_backing.h"
#include "src/__support/OSUtil/windows/memory/interval_skiplist.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/memory/skiplist_link_traits.h"
#include "src/__support/OSUtil/windows/memory/va_region_desc.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/error_or.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_addr.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
// File-scope state.
//===----------------------------------------------------------------------===//

namespace {
cpp::Atomic<uint32_t> g_init_done{0};
} // namespace

//===----------------------------------------------------------------------===//
// ART key encoding.
//===----------------------------------------------------------------------===//

// Big-endian 8-byte key. The high 4 bytes are zero by construction (47-bit
// user VA) but written anyway to keep `kArtKeyLen = 8` structurally fixed.
LIBC_INLINE void encode_art_key(uintptr_t va, uint8_t *out_8) {
    uint64_t high = static_cast<uint64_t>(va >> 32);
    out_8[0] = static_cast<uint8_t>((high >> 56) & 0xFFu);
    out_8[1] = static_cast<uint8_t>((high >> 48) & 0xFFu);
    out_8[2] = static_cast<uint8_t>((high >> 40) & 0xFFu);
    out_8[3] = static_cast<uint8_t>((high >> 32) & 0xFFu);
    out_8[4] = static_cast<uint8_t>((high >> 24) & 0xFFu);
    out_8[5] = static_cast<uint8_t>((high >> 16) & 0xFFu);
    out_8[6] = static_cast<uint8_t>((high >> 8) & 0xFFu);
    out_8[7] = static_cast<uint8_t>(high & 0xFFu);
}

LIBC_INLINE uintptr_t leaf_va_prefix(uintptr_t va) {
    return va & ~static_cast<uintptr_t>(0xFFFFFFFFu);
}

// LoadKey for ART optimistic-prefix tail validation and lazy leaf expansion;
// recovers a leaf's canonical key from the tagged `Arena *`. SysV — libc-
// internal, never crosses the NT boundary.
void va_tracker_load_key_for_arena(Arena *leaf, uint8_t out_key[kArtKeyLen]) {
    if (LIBC_UNLIKELY(leaf == nullptr)) {
        for (uint32_t i = 0; i < kArtKeyLen; ++i)
            out_key[i] = 0;
        return;
    }
    encode_art_key(leaf->arena_lo, out_key);
}

//===----------------------------------------------------------------------===//
// Range validation and arena resolution.
//===----------------------------------------------------------------------===//

constexpr uintptr_t kPageGranularity = 4u * 1024u;

// Page-aligned (not 64 KiB) so post-`split` sub-ranges are accepted.
[[nodiscard]] LIBC_INLINE bool range_valid_interior(VaRange r) {
    if (r.bytes == 0)
        return false;
    uintptr_t lo = r.lo();
    if ((lo & (kPageGranularity - 1)) != 0)
        return false;
    if ((r.bytes & (kPageGranularity - 1)) != 0)
        return false;
    uintptr_t hi = lo + r.bytes;
    if (hi < lo) // wraparound past 64-bit
        return false;
    return true;
}

[[nodiscard]] Arena *resolve_arena_for_va(uintptr_t va) {
    uint8_t key[kArtKeyLen];
    encode_art_key(va, key);
    return art_lookup(g_art_tree, key, kArtKeyLen);
}

// The kernel embeds the linear index in `IA32_TSC_AUX` on every context
// switch, so this syscall folds to RDPID (or RDTSCP) — no kernel transition.
[[nodiscard]] uint32_t current_cpu_index() {
    PROCESSOR_NUMBER pn{};
    (void)::NtGetCurrentProcessorNumberEx(&pn);
    return static_cast<uint32_t>(pn.Group) * 64u +
           static_cast<uint32_t>(pn.Number);
}

[[nodiscard]] Arena *resolve_or_install_arena(uintptr_t va) {
    uint8_t key[kArtKeyLen];
    encode_art_key(va, key);

    if (Arena *existing = art_lookup(g_art_tree, key, kArtKeyLen))
        return existing;

    // No arena yet — allocate, CAS, retire-and-relookup on peer-win.
    uintptr_t arena_lo = leaf_va_prefix(va);
    Arena *fresh = arena_alloc(arena_lo, current_cpu_index());
    if (LIBC_UNLIKELY(fresh == nullptr))
        return nullptr;

    if (art_insert(g_art_tree, key, kArtKeyLen, fresh))
        return fresh;

    arena_retire(fresh);
    return art_lookup(g_art_tree, key, kArtKeyLen);
}

//===----------------------------------------------------------------------===//
// Walk adapter — bridges the skiplist visitor signature to `WalkVisitor`.
//===----------------------------------------------------------------------===//

namespace {

struct WalkAdapter {
    WalkVisitor user_visitor{nullptr};
    void *user_ctx{nullptr};

    LIBC_INLINE void operator()(SkiplistNodeBase *node) {
        if (user_visitor == nullptr || node == nullptr)
            return;
        // Pairs with the mutation envelope's RELEASE store of `value`
        // (cross-TU — partner not visible locally).
        RegionDesc *rd = node->value.load(cpp::MemoryOrder::ACQUIRE);
        VaRange covered{reinterpret_cast<void *>(node->lo),
                        static_cast<size_t>(node->hi - node->lo)};
        user_visitor(covered, rd, user_ctx);
    }
};

} // namespace

//===----------------------------------------------------------------------===//
// resolve — wait-free SIGSEGV-callable.
//===----------------------------------------------------------------------===//

::LIBC_NAMESPACE::ErrorOr<RegionRef> resolve(void *addr) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    Arena *arena = resolve_arena_for_va(a);
    if (LIBC_UNLIKELY(arena == nullptr))
        return ::LIBC_NAMESPACE::Error{ENOENT};

    // Returned desc is pinned on `kPinSlotCur` until the next va_tracker
    // call on this thread.
    RegionDesc *desc = Query(arena, a);
    if (desc == nullptr)
        return ::LIBC_NAMESPACE::Error{ENOENT};

    RegionRef ref;
    ref.desc = desc;
    return ref;
}

//===----------------------------------------------------------------------===//
// walk_range.
//===----------------------------------------------------------------------===//

void walk_range(VaRange range, WalkVisitor visitor, void *ctx) {
    if (visitor == nullptr || !range_valid_interior(range))
        return;

    Arena *arena = resolve_arena_for_va(range.lo());
    if (arena == nullptr)
        return;

    WalkAdapter adapter;
    adapter.user_visitor = visitor;
    adapter.user_ctx = ctx;
    is_walk_range(arena, range.lo(), range.hi(), adapter);
}

//===----------------------------------------------------------------------===//
// Fork — serialize / replay / reinit.
//===----------------------------------------------------------------------===//

namespace {

// Per-Arena visitor. Stops emitting after the first non-zero `sink.emit`.
struct SerializeIntervalVisitor {
    ForkSink *sink{nullptr};
    int last_err{0};

    LIBC_INLINE void operator()(SkiplistNodeBase *node) {
        if (last_err != 0 || sink == nullptr || node == nullptr)
            return;
        RegionDesc *rd = node->value.load(cpp::MemoryOrder::ACQUIRE);
        if (rd == nullptr)
            return;

        VaRange r{reinterpret_cast<void *>(node->lo),
                  static_cast<size_t>(node->hi - node->lo)};
        RegionShape s = rd->current_shape();
        const uint16_t cur_flags = rd->flags_load();

        RegionKind kind = RegionKind::AnonPrivate;
        switch (s) {
        case RegionShape::FILE_VIEW_MONO:
        case RegionShape::FILE_VIEW_CHUNKED:
        case RegionShape::FILE_VIEW_RESERVE:
            kind = (cur_flags & region_flag::SHARED) != 0
                       ? RegionKind::FileShared
                       : RegionKind::FilePrivate;
            break;
        case RegionShape::ANON_PLACEHOLDER:
        case RegionShape::ANON_RESERVE_SECTION:
            kind = (cur_flags & region_flag::SHARED) != 0
                       ? RegionKind::AnonShared
                       : RegionKind::AnonPrivate;
            break;
        default:
            return; // not POSIX-visible — child rebuilds via loader/bootstrap
        }

        AcquireMeta meta;
        // Cross-domain read: the skiplist pin from `is_walk_range` does
        // not extend across Crystalline-W domains, so anchor the backing
        // domain's reader pin and wrap the multi-field read in a
        // BackingView whose per-load generation re-check traps any peer
        // kill+recycle that lands between successive loads. A reaper-
        // stored null is fine — the child re-acquires fresh handles.
        anchor_backing_reader_pin();
        BackingView b{deref_backing_raw(rd->backing_ref)};
        if (b) {
            meta.section_handle =
                b.load<&DescBacking::section_handle>(cpp::MemoryOrder::ACQUIRE);
            meta.file_handle =
                b.load<&DescBacking::file_handle>(cpp::MemoryOrder::ACQUIRE);
            meta.placeholder_base =
                b.load<&DescBacking::placeholder_base>(cpp::MemoryOrder::ACQUIRE);
            meta.placeholder_size =
                static_cast<size_t>(b.read<&DescBacking::placeholder_pages>()) *
                static_cast<size_t>(kPageGranularity);
        }
        meta.section_offset =
            static_cast<uint64_t>(rd->section_offset.QuadPart);
        // POSIX: memory locks are not inherited across fork(). Child
        // re-issues `mlock2(MLOCK_ONFAULT)` for the same posture.
        meta.flags = static_cast<uint16_t>(
            cur_flags & ~region_flag::LOCK_ONFAULT);

        // Walk MBI to build the protection map. Structural invariant:
        // every entry has >= 1 run and runs[0].prot equals meta.view_prot.
        // Fallbacks preserving the invariant: RegionWalker scratch-alloc
        // failure -> one run carrying acquire-intent; run-count overflow
        // -> collapse to one run with runs[0].prot.
        const uintptr_t desc_lo = node->lo;
        const size_t desc_bytes = static_cast<size_t>(node->hi - node->lo);
        ProtectionRun runs[kMaxProtectionRunsPerEntry];
        uint32_t run_count = 0;
        bool overflow = false;
        {
            nt_pal::RegionWalker walker(
                reinterpret_cast<void *>(desc_lo),
                static_cast<SIZE_T>(desc_bytes));
            if (walker) {
                while (walker.next()) {
                    if (run_count >= kMaxProtectionRunsPerEntry) {
                        overflow = true;
                        break;
                    }
                    const uintptr_t run_lo =
                        reinterpret_cast<uintptr_t>(walker.chunk);
                    runs[run_count].offset_from_range_lo =
                        static_cast<uint32_t>(run_lo - desc_lo);
                    runs[run_count].bytes =
                        static_cast<uint32_t>(walker.chunk_size);
                    runs[run_count].prot = walker.entry->Protect;
                    runs[run_count].reserved_ = 0;
                    ++run_count;
                }
            }
        }
        if (run_count == 0) {
            runs[0].offset_from_range_lo = 0;
            runs[0].bytes = static_cast<uint32_t>(desc_bytes);
            runs[0].prot = rd->view_prot;
            runs[0].reserved_ = 0;
            run_count = 1;
        } else if (overflow) {
            // Collapse to uniform rather than a partial map.
            runs[0].bytes = static_cast<uint32_t>(desc_bytes);
            run_count = 1;
        }

        meta.view_prot = runs[0].prot;

        last_err = sink->emit(sink->ctx, r, kind, meta, runs, run_count);
    }
};

struct ArtSerializeCtx {
    ForkSink *sink{nullptr};
    int last_err{0};
};

void art_serialize_visitor(const uint8_t * /*key*/, uint32_t /*key_len*/,
                           Arena *leaf, void *ctx_p) {
    if (leaf == nullptr || ctx_p == nullptr)
        return;
    auto *ctx = static_cast<ArtSerializeCtx *>(ctx_p);
    if (ctx->last_err != 0 || ctx->sink == nullptr)
        return;

    SerializeIntervalVisitor v;
    v.sink = ctx->sink;
    is_walk_range(leaf, leaf->arena_lo, leaf->arena_hi, v);
    if (v.last_err != 0)
        ctx->last_err = v.last_err;
}

} // namespace

int serialize_for_fork(ForkSink &sink) {
    if (sink.emit == nullptr)
        return EINVAL;

    // Whole key space: 0x00..00 to 0xFF..FF covers every VA prefix.
    uint8_t lo_key[kArtKeyLen] = {};
    uint8_t hi_key[kArtKeyLen];
    for (uint32_t i = 0; i < kArtKeyLen; ++i)
        hi_key[i] = 0xFFu;

    ArtSerializeCtx ctx;
    ctx.sink = &sink;
    (void)art_walk_range(g_art_tree, lo_key, hi_key, kArtKeyLen,
                         &art_serialize_visitor, &ctx);

    if (ctx.last_err != 0)
        // Normalise to positive errno — emit callbacks may return either
        // convention.
        return ctx.last_err > 0 ? ctx.last_err : -ctx.last_err;
    return 0;
}

int replay_in_child(const ForkSnapshot &snap) {
    if (snap.entries == nullptr && snap.count > 0)
        return EINVAL;

    for (size_t i = 0; i < snap.count; ++i) {
        const auto &e = snap.entries[i];
        auto r = acquire(e.range, e.kind, e.meta);
        if (!r.has_value())
            return r.error();
        // `acquire` committed with runs[0].prot via meta.view_prot;
        // apply remaining runs to reproduce per-page divergence.
        if (e.protection_count > 1) {
            const uintptr_t base = e.range.lo();
            for (uint32_t k = 1; k < e.protection_count; ++k) {
                void *p = reinterpret_cast<void *>(
                    base + e.protection_runs[k].offset_from_range_lo);
                if (!nt_pal::protect(p, e.protection_runs[k].bytes,
                                     e.protection_runs[k].prot))
                    return EFAULT;
            }
        }
    }
    return 0;
}

void va_tracker_fork_reinit() {
    // Crystalline-W discipline (Nikolaev, Ravindran - PLDI 2024 §1):
    // grace eras do not survive fork — reset all slot eras to zero and
    // discard inherited retire batches.
    g_va_tracker_art_domain.clear_all();
    g_va_tracker_skiplist_domain.clear_all();
    g_va_tracker_backing_domain.clear_all();

    art_index_fork_reinit();
    interval_skiplist_fork_reinit();
    backing_fork_reinit();
}

LIBC_REGISTER_FORK_REINIT(va_tracker,
                          ::LIBC_NAMESPACE::internal::kForkPrioVaTracker,
                          &va_tracker_fork_reinit)

//===----------------------------------------------------------------------===//
// Pre-fork barrier.
//===----------------------------------------------------------------------===//

void pre_fork_drain() {
    // Stale era pinned across the clone would leak memory until the
    // child's fork-reinit clears it.
    g_va_tracker_skiplist_domain.clear_all();
    g_va_tracker_art_domain.clear_all();
    g_va_tracker_skiplist_chunk_domain.clear_all();
    g_va_tracker_art_chunk_domain.clear_all();
    g_va_tracker_backing_domain.clear_all();
}

//===----------------------------------------------------------------------===//
// Bootstrap.
//===----------------------------------------------------------------------===//

bool is_va_tracker_ready() {
    return g_init_done.load(cpp::MemoryOrder::ACQUIRE) != 0;
}

uint32_t va_tracker_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                            uint32_t /*cap*/) {
    // ACQ_REL publishes init writes to subsequent readers AND acquires
    // any prior fork-reinit completion.
    if (g_init_done.exchange(1, cpp::MemoryOrder::ACQ_REL) != 0)
        return 0;

    g_va_tracker_art_domain.init_registration();
    g_va_tracker_skiplist_domain.init_registration();

    art_index_init(&va_tracker_load_key_for_arena);
    interval_skiplist_init();

    backing_init();

    // Warm-claim a slot in every registered Crystalline-W domain on this
    // thread — first `protect()` on a fault path would otherwise demand-
    // commit and risk recursive faulting inside the SIGSEGV handler.
    ::LIBC_NAMESPACE::concurrent::registry_warm_thread_all();

    (void)out;
    return 0;
}

LIBC_REGISTER_MEMORY_PRIMITIVE(va_tracker, 6, &va_tracker_init_fn)

//===----------------------------------------------------------------------===//
// Diagnostics.
//===----------------------------------------------------------------------===//

VaTrackerStats va_tracker_stats_snapshot() {
    VaTrackerStats s{};
    for (uint32_t i = 0; i < 4; ++i) {
        auto t = static_cast<ArtNodeType>(i);
        auto info = art_index_stats(t);
        s.art_live_chunks_per_type[i] = info.live_chunks;
        s.art_live_nodes_per_type[i] = info.live_nodes;
    }
    SkiplistStats sl = stats_snapshot();
    for (uint32_t i = 0; i < kBucketCount; ++i) {
        s.skiplist_live_chunks_per_bucket[i] = sl.live_chunks_per_bucket[i];
        s.skiplist_live_nodes_per_bucket[i] = sl.nodes_allocated_per_bucket[i];
    }
    s.arena_count = sl.arena_count;
    return s;
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
