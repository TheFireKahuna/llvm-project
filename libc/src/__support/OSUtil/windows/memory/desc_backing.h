//===- desc_backing.h - RegionDesc backing allocator ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One backing == one kernel reservation (placeholder base + page count, plus
// optional section and file handles). Multiple RegionDescs that share a
// reservation — fragments from partial unmap, MAP_FIXED carve, or mprotect
// shape changes — all encode the same BackingRef, so the kernel placeholder
// is never split and handles are never duplicated. Lifetimes are 1:1 with
// the source mapping, not with descriptor count.
//
// Two lifetimes co-exist on one slot:
//
//   Body — Crystalline-W (Nikolaev & Ravindran, PLDI 2024) via
//   g_va_tracker_backing_domain. Pinned readers keep the slot alive across
//   grace; slots recycle only after every reader's pin drains.
//
//   Kernel state — mutator-owned, synchronous. Transaction commit's
//   post-Swap survivor walk CASes state Live -> Killed when no LIVE desc
//   references the backing; the CAS winner runs backing_kill_and_retire,
//   which nulls and closes handles, nulls and frees the placeholder, then
//   retires the body. The CAS is one-way and set-once. Crystalline-W is
//   asynchronous and exposes no synchronous grace primitive, so kernel
//   teardown inside the FreeFn is structurally unachievable — it has to
//   live on the mutator path.
//
// Reader contract: pinned readers read DescBacking fields under their pin
// then call into the kernel via the captured handles/base. If the mutator
// has already torn the VA down, the NT call fails with
// STATUS_INVALID_HANDLE / STATUS_NOT_MAPPED_VIEW, which the best-effort
// consumers tolerate. The pin prevents a different mapping's identity
// landing in the slot mid-window.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_DESC_BACKING_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_DESC_BACKING_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

// One-way Live -> Killed lifecycle. The Killed transition is owned by the
// transaction whose post-Swap survivor walk concludes no LIVE desc still
// references the backing. The CAS to Killed is the linearisation point
// declaring teardown ownership — only the CAS winner may call
// `backing_kill_and_retire`; parallel "best-effort kill" paths would race
// teardown. There is no return path; recycled slots come back with a
// refreshed generation that fails any stale ref's triple-validate.
inline constexpr uint8_t kBackingStateLive = 0;
inline constexpr uint8_t kBackingStateKilled = 1;

// Drives teardown dispatch in backing_kill_and_retire. Section-view backings
// must unmap_view_preserve before free_placeholder (free on a still-mapped
// view returns STATUS_UNABLE_TO_DELETE_SECTION); private-commit skips the
// unmap (would return STATUS_NOT_MAPPED_VIEW).
//
// Rejected alternative: dispatch on section_handle != nullptr. A sibling
// re-map produced by partial-unmap/carve can be a section-view backing whose
// section_handle is null (the original keeps the handle; the sibling view
// stays alive via the kernel's internal section reference). Handle-presence
// dispatch would orphan those views.
enum class BackingShape : uint8_t {
    PrivateCommit = 0,
    SectionView = 1,
};

// Exactly one cache line; standard-layout so the static_assert wall below
// can pin every member offset. Layout:
//
//   [ 0..19] CrystallineNode runtime (batch_link u32 at 16)
//   [20..23] generation        — ABA defence; bound into BackingRef.
//   [24]     state             — Live(0) / Killed(1). One-way.
//   [25]     cached_chunk_id   — chunk_id of this slot.
//   [26]     cached_slot_idx   — slot index within the chunk.
//   [27]     shape             — PrivateCommit / SectionView.
//   [28..31] placeholder_pages — placeholder size in 4 KiB units.
//   [32..39] node_canary       — per-slot canary, triple-validated.
//   [40..47] placeholder_base  — atomic; nulled at teardown.
//   [48..55] section_handle    — atomic; nulled at teardown.
//   [56..63] file_handle       — atomic; nulled at teardown.
//
// Teardown atomics at offsets 40/48/56 are RELEASE-stored to nullptr by
// backing_kill_and_retire before the matching kernel primitive runs. A
// reader that captures a handle between the null-store and the close holds
// a still-valid handle until the close completes (subsequent NT calls then
// return STATUS_INVALID_HANDLE); a reader arriving after the null-store
// sees nullptr and skips. The Crystalline pin prevents the slot recycling
// underneath the reader either way.
//
// Invariants beyond what the types carry:
//   * Slot is trivially destructible — recycling memsets the bytes.
//   * state only transitions Live -> Killed.
//   * cached_chunk_id, cached_slot_idx, placeholder_pages, node_canary, and
//     shape are write-once; cross-thread happens-before is supplied by the
//     descriptor publish (Swap CAS on pred->next[0]), not by an atomic store
//     on these fields.
struct alignas(64) DescBacking
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // Intrusive Crystalline-W fields at [0..19]; emitted via macro so the
    // class stays standard-layout and offsetof() below is well-defined.
    LIBC_CRYSTALLINE_NODE_FIELDS(DescBacking);

    // Naturally packs into the 4-byte tail-pad after batch_link.
    cpp::Atomic<uint32_t> generation{0};

    // Winning Live -> Killed CAS is ACQ_REL; the kill-side entry assert
    // reads ACQUIRE; the provisional-rollback store is RELAXED because the
    // rollback owner has never published the backing and races no one.
    // Kill CAS site: `reap_old_backings` in va_tracker_execute.cpp.
    cpp::Atomic<uint8_t> state{};

    uint8_t cached_chunk_id{0};
    uint8_t cached_slot_idx{0};
    BackingShape shape{BackingShape::PrivateCommit};

    // Placeholder spans [placeholder_base, +placeholder_pages * 4 KiB) and
    // may exceed any single referencing descriptor's (lo, hi) range — brk
    // reserves a wide cursor, aligned reservations carry kernel-rounded
    // size, 4 KiB-granular post-split survivors carry exact byte length.
    // Extending uses a fresh backing rather than mutating this field.
    // u32 * 4 KiB caps at 16 TiB, well above the 47-bit user VA ceiling.
    uint32_t placeholder_pages{0};

    // Per-slot canary derived from partition_secret XOR class_id XOR
    // chunk_id XOR slot_idx. partition_secret lives in Zone 0b, is
    // ProcessPrng-derived, and is never observable to user code, so heap-
    // spray into a freed-but-unreused slot cannot forge it.
    uint64_t node_canary{0};

    // Atomic so the reaper can RELEASE-store nullptr before the matching
    // nt_pal::free_placeholder.
    cpp::Atomic<void *> placeholder_base{nullptr};

    // nullptr for anonymous-private mappings (no section exists). Reaper
    // skips NtClose on any handle whose RELEASE-loaded prior value is null.
    cpp::Atomic<HANDLE> section_handle{nullptr};

    // nullptr for pagefile-backed sections (no underlying file).
    cpp::Atomic<HANDLE> file_handle{nullptr};
};

// Layout pins. The encoded offsets feed BatchLinkCodec and the kill-side
// reaper's direct field math; a shift in the CrystallineNode header layout
// or Itanium/MSVC tail-padding reuse trips this wall and forces a re-audit
// of every derived class that packs into bytes [20..23].
static_assert(sizeof(DescBacking) == 64, "one cache line");
static_assert(alignof(DescBacking) == 64, "cache-line aligned");
static_assert(__is_trivially_destructible(DescBacking),
              "slot recycling memsets the bytes");
static_assert(offsetof(DescBacking, generation) == 20,
              "must reuse CrystallineNode tail-pad");
static_assert(offsetof(DescBacking, state) == 24, "state @ 24");
static_assert(offsetof(DescBacking, cached_chunk_id) == 25,
              "cached_chunk_id @ 25");
static_assert(offsetof(DescBacking, cached_slot_idx) == 26,
              "cached_slot_idx @ 26");
static_assert(offsetof(DescBacking, shape) == 27, "shape @ 27");
static_assert(sizeof(BackingShape) == 1, "shape must fit one byte");
static_assert(offsetof(DescBacking, placeholder_pages) == 28,
              "placeholder_pages @ 28");
static_assert(offsetof(DescBacking, node_canary) == 32, "node_canary @ 32");
static_assert(offsetof(DescBacking, placeholder_base) == 40,
              "placeholder_base @ 40");
static_assert(offsetof(DescBacking, section_handle) == 48,
              "section_handle @ 48");
static_assert(offsetof(DescBacking, file_handle) == 56, "file_handle @ 56");

// Opaque 64-bit ref:
//   bits  0..15 — slot_idx within the chunk
//   bits 16..31 — chunk_id within the partition pool
//   bits 32..63 — generation snapshotted at alloc time
//
// All-zero is the null sentinel; seed_generation_for_slot rejects 0 so a
// fresh (chunk=0, slot=0) ref can't collide. Dereference triple-validates
// (bounds, canary, generation) and traps on mismatch — a stale or recycled
// ref is a control-flow violation, not a recoverable error.
using BackingRef = uint64_t;
inline constexpr BackingRef kBackingRefNull = 0;

[[nodiscard]] LIBC_INLINE BackingRef
make_backing_ref(uint16_t chunk_id, uint16_t slot_idx, uint32_t generation) {
    return (static_cast<uint64_t>(generation) << 32) |
           (static_cast<uint64_t>(chunk_id) << 16) |
           static_cast<uint64_t>(slot_idx);
}

[[nodiscard]] LIBC_INLINE uint16_t backing_ref_chunk_id(BackingRef r) {
    return static_cast<uint16_t>((r >> 16) & 0xFFFFu);
}
[[nodiscard]] LIBC_INLINE uint16_t backing_ref_slot_idx(BackingRef r) {
    return static_cast<uint16_t>(r & 0xFFFFu);
}
[[nodiscard]] LIBC_INLINE uint32_t backing_ref_generation(BackingRef r) {
    return static_cast<uint32_t>((r >> 32) & 0xFFFFFFFFu);
}

// Crystalline-W FreeFn. Body cleanup only — must not call nt_pal::* or
// NtClose; kernel state was torn down synchronously by
// backing_kill_and_retire on the mutator path. CI grep gate enforces it.
// Crystalline-W (paper §1) has no synchronous grace primitive, so kernel
// teardown from inside a FreeFn is structurally unachievable, not merely
// slow.
void desc_backing_free(DescBacking *backing);

inline constexpr uint32_t kBackingRetireFreq = 16;

// MaxIdx = max(BackingPinSlot::*) + 1.
inline constexpr uint32_t kBackingMaxIdx = 2;

extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    DescBacking, &desc_backing_free, kBackingRetireFreq, kBackingMaxIdx>
    g_va_tracker_backing_domain;

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

// Returns a Live, canary-stamped, zero-kernel-state slot with a non-zero
// generation seeded from partition_secret (rotated on fork). Returns
// nullptr on partition/pool exhaustion. Caller is sole owner until the
// transaction's Swap CAS publishes a desc that references this backing;
// caller must push the result onto the transaction's provisional list so
// a STEP 2 / STEP 3 failure can tear it down without leaking kernel state.
[[nodiscard]] DescBacking *backing_alloc();

// Populate kernel state once, immediately after backing_alloc, inside the
// transaction's nt_pal phase and before the referencing desc is published.
// Stores are RELEASE so a future reader who arrives through the published
// desc observes the populated fields. Handles first, base last; the
// teardown null-stores follow the same order so a "load handle, close
// handle" reader never sees a closed handle with a non-null base.
void backing_set_kernel_state(DescBacking *backing,
                              void *placeholder_base,
                              uint32_t placeholder_pages,
                              BackingShape shape,
                              HANDLE section_handle,
                              HANDLE file_handle);

// Synchronous kernel-state teardown; called by the post-Swap survivor walk
// after the caller wins the Live -> Killed CAS (or by rollback with a
// RELAXED Killed store for a never-published backing). In order:
//
//   1. RELEASE-store nullptr to section_handle, file_handle, and
//      placeholder_base, capturing prior values.
//   2. Walk the claimed extent [saved_base, saved_base+pages*4K). For
//      each non-MEM_FREE region whose AllocationBase lies in-range:
//      unmap_view_preserve (SectionView shape, MEM_MAPPED type) then
//      free_placeholder(AllocationBase). The walk handles fragmented
//      extents left behind by a failed-rollback PrivateCommit OW
//      replace (L_committed + M_FREE + R_committed); the legacy
//      single-call free at saved_base would orphan R.
//   3. NtClose the captured section_handle and file_handle if non-null.
//   4. g_va_tracker_backing_domain.retire(backing).
//
// On return the claimed extent's in-range VADs are MEM_FREE; the slot
// memory lingers under Crystalline grace until reader pins drain. Entry
// assert requires state == Killed under ACQUIRE.
//
// Implementation lives in va_tracker_execute.cpp so this file's FreeFn
// TU stays free of nt_pal::* / NtClose / split_placeholder — CI grep gate
// enforces it.
void backing_kill_and_retire(DescBacking *backing);

// Metadata-only retire — Killed-state store + Crystalline retire,
// no nt_pal:: calls. For sibling backings created by `edge_remap_one`:
// their `placeholder_base` names a VA fragment of a wider OLD backing
// that on a rollback path is still LIVE and still claims the full
// extent, so the sibling's lifecycle ends without touching the
// placeholder or any (null-by-construction) handles. Dispatched via
// `ProvisionalList::Kind::Sibling` in `rollback_provisional` — an
// on-backing flag cleared after Swap publishes the sibling LIVE races
// a concurrent envelope reaching the backing through the new chain
// before the clear synchronizes, so the distinction stays private to
// the envelope owner thread.
void backing_retire_metadata_only(DescBacking *backing);

//===----------------------------------------------------------------------===//
// Cross-domain pin defence — anchor pins / BackingView
//===----------------------------------------------------------------------===//
//
// Crystalline-W pins are not transitive across domains: a skiplist pin on a
// RegionDesc does not cover the DescBacking the desc points at. Two
// disciplines exist for taking the second pin:
//
//   Engine path (run_envelope and the per-op execute / reaper helpers).
//   anchor_backing_engine_pin() once at envelope entry. LockedSet inside
//   the envelope holds locks on every desc in the working range; a peer
//   cannot kill a backing whose desc is locked Live, because the kill
//   protocol requires no LIVE desc references the backing. Lock-set plus
//   anchor pin exclude both peer-mid-read-kill and recycle — raw deref is
//   safe per-call.
//
//   Reader path (fork serializer, VEH probes, future msync / madvise /
//   numa_ops). anchor_backing_reader_pin() at the read site. No LockedSet:
//   peers can kill and recycle during the read. The pin fixes the era but
//   not inter-field staleness within the pinned scope (preempted between
//   two field loads while a peer killed-and-recycled). BackingView's
//   per-load generation re-check is the structural defence.
//
// Crystalline-W protect() is non-cumulative per (thread, slot index) and
// has no release primitive — the next protect() on the same index, or
// clear_all(), or thread exit reclaims it. Anchor functions are therefore
// one-shot with no destructor.
namespace BackingPinSlot {
inline constexpr uint32_t kEngineAnchor = 0;
inline constexpr uint32_t kReaderPin = 1;
// Disjoint slot indices stop engine and reader from rotating each other's
// pin via the non-cumulative per-index semantics.
static_assert(kEngineAnchor != kReaderPin, "slots must be disjoint");
} // namespace BackingPinSlot

// Covers every per-op execute and reaper raw deref through the entire
// run_envelope retry loop; sibling va_tracker calls inside the envelope do
// not rotate this slot.
void anchor_backing_engine_pin();

// Pinned scope extends until the next protect() on kReaderPin on this
// thread (typically the next call here), clear_all(), or thread exit. A
// grep over this symbol enumerates every cross-domain reader entry in the
// tree. Pair with BackingView for field reads — the pin alone does not
// catch inter-field staleness.
void anchor_backing_reader_pin();

// Generation-checking view. Defends against kill+recycle slipping through
// between successive field reads on the reader path. Snapshots generation
// at construction; every load reloads generation post-field-read and traps
// on mismatch — a slot whose generation advanced is a different incarnation
// and the trap fires before the caller acts on stale fields.
//
// Reader path only. Engine callers under anchor_backing_engine_pin +
// LockedSet already exclude the race this defends, so wrapping engine
// accesses would add cost without safety.
class BackingView {
public:
    // ACQUIRE snapshot pairs with the alloc-side RELEASE generation seed.
    // Null in -> null view; subsequent loads on a null view trap.
    LIBC_INLINE explicit BackingView(DescBacking *b) noexcept : b_(b) {
        pinned_gen_ =
            b == nullptr
                ? 0
                : b->generation.load(cpp::MemoryOrder::ACQUIRE);
    }

    LIBC_INLINE BackingView() noexcept : b_(nullptr), pinned_gen_(0) {}

    [[nodiscard]] LIBC_INLINE explicit operator bool() const noexcept {
        return b_ != nullptr;
    }

    // Identity only; does not re-validate generation. Useful for comparing
    // against a sibling deref_backing_raw under the engine anchor.
    [[nodiscard]] LIBC_INLINE bool operator==(const DescBacking *o) const
        noexcept {
        return b_ == o;
    }
    [[nodiscard]] LIBC_INLINE bool operator!=(const DescBacking *o) const
        noexcept {
        return b_ != o;
    }

    // Atomic-field load under the snapshotted generation. Reloads generation
    // post-load and traps on mismatch — kill+recycle between snapshot and
    // field load is a hard error, not a soft retry.
    template <auto MemberPtr>
    [[nodiscard]] LIBC_INLINE auto load(cpp::MemoryOrder mo) const {
        if (LIBC_UNLIKELY(b_ == nullptr))
            __builtin_trap();
        auto v = (b_->*MemberPtr).load(mo);
        if (LIBC_UNLIKELY(b_->generation.load(cpp::MemoryOrder::ACQUIRE) !=
                          pinned_gen_))
            __builtin_trap();
        return v;
    }

    // Plain-field variant for write-once members (shape, placeholder_pages,
    // cached_chunk_id, cached_slot_idx) whose cross-thread happens-before
    // is supplied by the descriptor publish, not by an atomic store.
    template <auto MemberPtr>
    [[nodiscard]] LIBC_INLINE auto read() const {
        if (LIBC_UNLIKELY(b_ == nullptr))
            __builtin_trap();
        auto v = b_->*MemberPtr;
        if (LIBC_UNLIKELY(b_->generation.load(cpp::MemoryOrder::ACQUIRE) !=
                          pinned_gen_))
            __builtin_trap();
        return v;
    }

private:
    DescBacking *b_;
    uint32_t pinned_gen_;
};

// Triple-validates bounds, canary, and generation; traps on any mismatch.
// Returns nullptr only when ref == kBackingRefNull.
//
// Caller must already hold a Crystalline pin on the backing domain via
// anchor_backing_engine_pin or anchor_backing_reader_pin — the pin is the
// recycle defence. Inter-field staleness defence (reader path only) is
// BackingView; raw deref does not address it.
[[nodiscard]] DescBacking *deref_backing_raw(BackingRef ref);

//===----------------------------------------------------------------------===//
// Bootstrap and fork hooks
//===----------------------------------------------------------------------===//

// One-shot; called from va_tracker_init_fn after skiplist init.
void backing_init();

// Drops every pin (clear_all), refreshes per-slot canaries against the
// rotated partition_secret, and reclaims pool slots whose pre-fork retire
// batches were stranded by a dead thread's Crystalline cell. Invoked at
// fork priority kForkPrioVaTracker and from pre_fork_drain.
void backing_fork_reinit();

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// BatchLinkCodec specialization
//===----------------------------------------------------------------------===//

// Required by every TU that instantiates CrystallineDomain<DescBacking,...>.
// Encodes ((chunk_id << 8) | slot_idx) + 1 into the substrate's 4-byte
// batch_link slot; +1 keeps 0 reserved as the unretired sentinel. Body
// lives in desc_backing.cpp so the decoder can reach
// g_backing_state.chunk_table[] for the chunk_id -> chunk_base lookup.

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::DescBacking> {
  static uint32_t encode(CrystallineNode *n) noexcept;
  static CrystallineNode *decode(uint32_t code) noexcept;
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_DESC_BACKING_H
