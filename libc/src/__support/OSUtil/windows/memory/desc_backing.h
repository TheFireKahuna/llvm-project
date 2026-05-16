//===- desc_backing.h - RegionDesc backing allocator ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Backing-object allocator and lifetime manager for the Layer 1 VA
/// tracker. Provides the cache-line-sized `DescBacking` body that
/// `RegionDesc` records point at via an opaque `BackingRef`, plus the
/// dedicated Crystalline-W domain that governs body reclamation.
///
/// One backing represents one kernel reservation: a placeholder
/// identity (base + page count in 64 KiB units), an optional section
/// handle, and an optional file handle. Several descriptors that share
/// a single reservation — fragments produced by partial unmap,
/// MAP_FIXED carve, or mprotect shape-changes — encode the same
/// `BackingRef`. The kernel placeholder is split zero times, the
/// kernel handles are duplicated zero times, and the lifetimes are
/// 1:1 with the source mapping rather than with the descriptor count.
///
/// Two lifetimes co-exist on one slot.
///
/// **Body lifetime.** Governed by Crystalline-W (Nikolaev and
/// Ravindran, PLDI 2024) through `g_va_tracker_backing_domain`.
/// Pinned readers (the VEH classifier, `msync` / `madvise` /
/// `numa_ops` consumers) keep the slot alive across grace; the slot
/// recycles only after every reader's pin drains. Standard SMR.
///
/// **Kernel-state lifetime.** Owned by the synchronous mutator path
/// on the va_tracker. A transaction's commit decides — by post-Swap
/// survivor walk — that no LIVE descriptor references this backing
/// any more and CASes `state` from Live to Killed. The single CAS
/// winner runs `backing_kill_and_retire` synchronously: nulls and
/// closes the handles, nulls and frees the placeholder, then retires
/// the body to Crystalline. The CAS is one-way and set-once. Failure
/// paths cannot leak — a transaction that fails after allocating a
/// backing walks its provisional list and tears down what it
/// privately owns. Crystalline-W is asynchronous and provides no
/// synchronous grace primitive, so kernel-state teardown inside a
/// FreeFn is structurally unachievable; it has to live on the
/// mutator path.
///
/// **Reader contract.** Pinned readers read `DescBacking` fields
/// under their pin, decide what to do, then dereference the
/// underlying VA only through a separate kernel call. They never
/// dereference user content directly. If the mutator has already
/// torn the VA down, the kernel call fails with
/// `STATUS_INVALID_HANDLE` / `STATUS_NOT_MAPPED_VIEW`, which the
/// best-effort consumers already tolerate. The Crystalline pin on
/// the backing prevents a different mapping's handles or base
/// landing in the slot mid-window.
///
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

/// State-byte values for the one-way Live -> Killed lifecycle.
///
/// Fresh allocations are always Live. The transition is owned by the
/// transaction whose commit determines that no surviving descriptor
/// references the backing; that commit also runs the synchronous
/// kernel-state teardown. There is no Killed -> Live transition.
/// Once Killed, the slot rides Crystalline grace to reclamation, and
/// any future allocation either lands in a fresh slot or in the same
/// slot post-grace with a refreshed generation that fails any stale
/// reference's triple-validate.
inline constexpr uint8_t kBackingStateLive = 0;
inline constexpr uint8_t kBackingStateKilled = 1;

/// Kernel-state shape tag carried on each `DescBacking`.
///
/// Drives the teardown dispatch in `backing_kill_and_retire`:
/// section-view backings must `unmap_view_preserve` before
/// `free_placeholder` (a free on a still-mapped view returns
/// `STATUS_UNABLE_TO_DELETE_SECTION`); private-commit backings skip
/// the unmap entirely (it would return `STATUS_NOT_MAPPED_VIEW` and
/// burn a syscall).
///
/// Shape is set once at `backing_set_kernel_state` time and never
/// mutated thereafter. A handle-presence dispatch
/// (`section_handle != nullptr`) is deliberately not used here: a
/// sibling re-map produced during a partial-unmap or carve operation
/// can create a section-view backing whose `section_handle` is null
/// (the original backing keeps the kernel handle while the sibling
/// view stays alive via the kernel's internal section reference).
/// A handle-presence gate would route those backings to the
/// no-unmap branch and orphan their views.
enum class BackingShape : uint8_t {
    PrivateCommit = 0,
    SectionView = 1,
};

/// Crystalline-W-managed backing for a Layer 1 RegionDesc body.
///
/// Exactly one cache line. Standard-layout under the Crystalline node
/// macros so that `offsetof` is well-defined on every member; layout
/// is pinned by the `static_assert` wall at the bottom of the class.
///
/// Layout summary (validated by `static_assert`):
/// \code
///   [ 0..19] CrystallineNode runtime fields (batch_link u32 at 16..19)
///   [20..23] generation        — ABA + recycled-slot defense; bound
///                                into BackingRef alongside slot_idx.
///   [24]     state             — Live (0) / Killed (1). One-way.
///   [25]     cached_chunk_id   — chunk_id of this slot (<=256).
///   [26]     cached_slot_idx   — slot index within the chunk (<=256).
///   [27]     shape             — PrivateCommit / SectionView.
///   [28..31] placeholder_pages — placeholder size in 4 KiB (NT page) units.
///   [32..39] node_canary       — per-slot canary, triple-validated.
///   [40..47] placeholder_base  — atomic; nulled at synchronous teardown.
///   [48..55] section_handle    — atomic; nulled at synchronous teardown.
///   [56..63] file_handle       — atomic; nulled at synchronous teardown.
/// \endcode
///
/// The kernel-state atomics at offsets 40, 48 and 56 are
/// RELEASE-stored to nullptr by `backing_kill_and_retire` before the matching kernel
/// primitive runs. A reader that captures a handle between the
/// null-store and the close holds a still-valid handle until the
/// close completes, after which subsequent NT calls return
/// `STATUS_INVALID_HANDLE`. A reader that arrives after the
/// null-store sees nullptr and skips. The Crystalline pin keeps the
/// slot from recycling underneath the reader.
///
/// Invariants:
///   * The slot is trivially destructible — recycling memsets the
///     bytes; no C++ object semantics may live inside it.
///   * `state` only transitions Live -> Killed.
///   * `cached_chunk_id`, `cached_slot_idx`, `placeholder_pages`,
///     `node_canary` and `shape` are write-once at allocation /
///     kernel-state-set time; the descriptor publish (Swap CAS on
///     `pred->next[0]`) provides the cross-thread happens-before for
///     any reader who arrives via the published desc.
struct alignas(64) DescBacking
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
    // [0..19] Intrusive Crystalline-W runtime fields emitted via the
    // macro so DescBacking is standard-layout and every offsetof()
    // below is well-defined.
    LIBC_CRYSTALLINE_NODE_FIELDS(DescBacking);

    /// Per-slot generation snapshot, bound into `BackingRef` to
    /// defeat ABA and stale-slot UAF on recycle. Naturally packs
    /// after the 4-byte batch_link at offset 16.
    cpp::Atomic<uint32_t> generation{0};

    /// Live / Killed lifecycle byte.
    ///
    /// The winning Live -> Killed CAS uses ACQ_REL; the assert at
    /// the top of `backing_kill_and_retire` reads it ACQUIRE; the
    /// provisional-rollback store uses RELAXED because the rollback
    /// owner has never published the backing and races no one.
    cpp::Atomic<uint8_t> state{};

    /// Cached chunk_id of this slot (<=256, fits in u8). Stamped
    /// once at `backing_alloc`; consumed by `encode_backing_ref` and
    /// `BatchLinkCodec::encode` to skip a pagemap_load_descriptor
    /// roundtrip. Plain (non-atomic): the cross-thread
    /// happens-before is supplied by the descriptor publish.
    uint8_t cached_chunk_id{0};

    /// Cached slot index within the chunk (<=256). Same write-once
    /// discipline as `cached_chunk_id`.
    uint8_t cached_slot_idx{0};

    /// Kernel-state shape. Drives the unmap-then-free vs free-only
    /// dispatch in `backing_kill_and_retire`. Set once at
    /// `backing_set_kernel_state` time. See `BackingShape` for why
    /// shape is separated from handle ownership.
    BackingShape shape{BackingShape::PrivateCommit};

    /// Placeholder size in 4 KiB (NT page) units. The placeholder covers
    /// `[placeholder_base, placeholder_base + placeholder_pages *
    /// 4 KiB)`. May exceed any single referencing descriptor's
    /// (lo, hi) range (brk reserves a wide cursor; aligned reservations
    /// carry their kernel-rounded actual size; 4 KiB-granular post-
    /// split survivors carry their exact byte length). Write-once at
    /// `backing_set_kernel_state` time; extending the placeholder is
    /// handled by allocating a fresh backing rather than mutating this
    /// field. `uint32_t` × 4 KiB caps a single backing's placeholder
    /// extent at 16 TiB, well above the 47-bit POSIX user VA ceiling.
    uint32_t placeholder_pages{0};

    /// Per-slot canary derived at alloc time from `partition_secret`
    /// XOR class_id XOR chunk_id XOR slot_idx. Validated in every
    /// `deref_backing_raw` before any other field read. An attacker with
    /// arbitrary write into a freed-but-unreused slot cannot guess
    /// `partition_secret` — it lives in Zone 0b of the PCB,
    /// ProcessPrng-derived, and is never observable to user code.
    uint64_t node_canary{0};

    /// Placeholder base VA. Atomic so the reaper can RELEASE-store
    /// nullptr before the matching `nt_pal::free_placeholder` runs.
    /// A reader that loads this between the null-store and the free
    /// observes a still-kernel-valid VA; a reader that loads after
    /// the free observes nullptr and skips.
    cpp::Atomic<void *> placeholder_base{nullptr};

    /// Section handle (or nullptr for anonymous-private mappings
    /// where no section exists). Same teardown ordering as
    /// `placeholder_base`. The reaper skips the close on any
    /// handle field whose RELEASE-loaded prior value is nullptr.
    cpp::Atomic<HANDLE> section_handle{nullptr};

    /// File handle (or nullptr for pagefile-backed sections, where
    /// no file underlies the section). Same teardown ordering.
    cpp::Atomic<HANDLE> file_handle{nullptr};
};

// Layout pins. If the CrystallineNode header layout shifts, this
// wall fires and every derived class that packs into bytes [20..23]
// must be re-audited (RegionDesc::view_prot is the other consumer).
static_assert(sizeof(DescBacking) == 64,
              "DescBacking must be exactly one cache line — slot size "
              "in PartitionClass::VaTrackerDescBacking depends on it");
static_assert(alignof(DescBacking) == 64,
              "DescBacking must be cache-line aligned");
static_assert(__is_trivially_destructible(DescBacking),
              "DescBacking must be trivially destructible — slot "
              "recycling memsets the bytes; no C++ object semantics "
              "may live inside the slot");

static_assert(offsetof(DescBacking, generation) == 20,
              "DescBacking::generation must reuse the CrystallineNode "
              "tail-pad slot at offset 20; if this fires, audit every "
              "derived class that packs into bytes [20..23] under the "
              "Itanium / MSVC tail-padding-reuse rule");
static_assert(offsetof(DescBacking, state) == 24,
              "DescBacking::state layout pin");
static_assert(offsetof(DescBacking, cached_chunk_id) == 25,
              "DescBacking::cached_chunk_id layout pin");
static_assert(offsetof(DescBacking, cached_slot_idx) == 26,
              "DescBacking::cached_slot_idx layout pin");
static_assert(offsetof(DescBacking, shape) == 27,
              "DescBacking::shape layout pin");
static_assert(sizeof(BackingShape) == 1,
              "BackingShape must fit the byte at offset 27");
static_assert(offsetof(DescBacking, placeholder_pages) == 28,
              "DescBacking::placeholder_pages layout pin");
static_assert(offsetof(DescBacking, node_canary) == 32,
              "DescBacking::node_canary layout pin");
static_assert(offsetof(DescBacking, placeholder_base) == 40,
              "DescBacking::placeholder_base layout pin");
static_assert(offsetof(DescBacking, section_handle) == 48,
              "DescBacking::section_handle layout pin");
static_assert(offsetof(DescBacking, file_handle) == 56,
              "DescBacking::file_handle layout pin");

/// Opaque 64-bit reference to a `DescBacking` slot.
///
/// Encoding:
/// \code
///   bits  0..15  — slot_idx within the chunk
///   bits 16..31  — chunk_id within the partition pool
///   bits 32..63  — generation snapshotted at alloc time
/// \endcode
///
/// All-zero is the null sentinel. A fresh allocation never lands at
/// `(chunk_id=0, slot_idx=0, generation=0)` simultaneously because
/// `seed_generation_for_slot` rejects 0 and bumps it to 1 before
/// publishing the ref.
///
/// Every dereference triple-validates: bounds first
/// (`chunk_id < kChunksPerBucket`, `slot_idx < kSlotsPerChunk`),
/// then canary match, then generation match. Each failure traps —
/// a stale or recycled reference is a control-flow violation, not
/// a recoverable state.
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

/// Crystalline-W FreeFn for `DescBacking`.
///
/// Body cleanup only — never calls `nt_pal::*`, never calls
/// `NtClose`. The kernel state was already torn down synchronously
/// inside `backing_kill_and_retire` on the mutator path.
/// A CI grep gate enforces this. The rationale is structural:
/// Crystalline-W is asynchronous (Nikolaev and Ravindran, PLDI 2024,
/// §1) and provides no synchronous grace primitive, so kernel
/// teardown from inside a FreeFn is not merely slow — it is
/// structurally unachievable.
///
/// Body: validates the per-slot canary, memsets the slot, and
/// releases it through the chunk-state-machine drain.
void desc_backing_free(DescBacking *backing);

inline constexpr uint32_t kBackingRetireFreq = 16;

/// MaxIdx for the desc_backing domain. Pin slots:
///   `BackingPinSlot::kEngineAnchor = 0` — engine path anchor for
///       `run_envelope` retry window.
///   `BackingPinSlot::kReaderPin    = 1` — reader path anchor across
///       `BackingView` lifetimes.
///
/// Audited max index = 1, so MaxIdx = 2.
inline constexpr uint32_t kBackingMaxIdx = 2;

extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    DescBacking, &desc_backing_free, kBackingRetireFreq, kBackingMaxIdx>
    g_va_tracker_backing_domain;

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// Allocate a fresh `DescBacking` from the VaTrackerDescBacking
/// partition.
///
/// On success the returned slot is `state == kBackingStateLive`,
/// kernel-state atomics are zeroed, the canary is stamped, and
/// `generation` is seeded from a `partition_secret`-derived mix
/// (rotated on every fork to avoid cross-fork aliasing). The caller
/// is the sole owner of the backing until it publishes it via the
/// transaction's Swap CAS; the caller must push the returned backing
/// onto its Transaction's provisional list so a STEP 2 / STEP 3
/// failure path can tear it down without leaking kernel state.
///
/// \returns the fresh backing, or nullptr on partition or pool
///          exhaustion.
[[nodiscard]] DescBacking *backing_alloc();

/// Populate the initial kernel-state on a freshly-allocated backing.
///
/// Called once, immediately after `backing_alloc`, inside the
/// transaction's `nt_pal` phase and before the descriptor that
/// references this backing is published via Swap. The stores are
/// RELEASE-ordered so that a future reader who arrives through the
/// published descriptor — and who pins the backing — observes the
/// populated fields. Field order is handles first, base last; Stage
/// 2's null-store follows the same order so a reader that does
/// "load handle, close handle" never sees a closed handle paired
/// with a non-null base.
void backing_set_kernel_state(DescBacking *backing,
                              void *placeholder_base,
                              uint32_t placeholder_pages,
                              BackingShape shape,
                              HANDLE section_handle,
                              HANDLE file_handle);

/// Synchronous kernel-state teardown.
///
/// Invoked from `Transaction::commit`'s post-Swap survivor walk after
/// the caller has won the `state` CAS Live -> Killed (or RELAXED-
/// stored Killed for a never-published provisional backing during
/// rollback). Steps in order:
///   1. RELEASE-store nullptr to `section_handle` and `file_handle`,
///      capturing the prior values.
///   2. `NtClose` the captured handles if non-null.
///   3. RELEASE-store nullptr to `placeholder_base`, capturing the
///      prior value.
///   4. `nt_pal::free_placeholder(saved_base)` — releases the entire
///      placeholder VAD; the `saved_pages` value is informational only.
///   5. `g_va_tracker_backing_domain.retire(backing)`.
///
/// Synchronous teardown — the kernel placeholder is `MEM_FREE` on
/// return; the slot memory lingers under Crystalline grace until
/// reader pins drain.
///
/// \pre `backing->state.load(ACQUIRE) == kBackingStateKilled`.
///      Reaching this function without first transitioning state is
///      a programming bug; the entry assertion traps.
///
/// Implementation lives in `va_tracker_transaction.cpp` so the
/// FreeFn translation unit (`desc_backing.cpp`) holds zero
/// `nt_pal::*` / `NtClose` calls. The CI grep gate
/// `nt_pal::|NtClose|NtDuplicateObject|split_placeholder` on
/// `desc_backing.cpp` returns clean.
void backing_kill_and_retire(DescBacking *backing);

//===----------------------------------------------------------------------===//
// Cross-domain pin defence — slot tags / anchor pins / BackingView
//===----------------------------------------------------------------------===//
//
// Pin transitivity does not extend across Crystalline-W domains
// (Nikolaev and Ravindran, PLDI 2024). A consumer holding a pin on
// `g_va_tracker_skiplist_domain` for a `RegionDesc` can dereference
// the desc, but following `desc->backing_ref` into
// `g_va_tracker_backing_domain` requires a separate pin on the
// backing domain.
//
// Two access disciplines exist:
//
// **Engine path (`run_envelope` and the per-op execute / reaper
// helpers it calls).** Anchors `BackingPinSlot::kEngineAnchor`
// once at envelope entry via `anchor_backing_engine_pin()`. Inside the
// envelope, `LockedSet` holds locks on every descriptor in the working
// range; a peer envelope cannot kill a backing whose descriptor is
// locked Live (the kill protocol requires no LIVE desc references the
// backing). Lock-set + anchor pin together exclude both
// peer-mid-read-kill and recycle, so raw access via
// `deref_backing_raw` is safe per-call.
//
// **Reader path (fork serializer, VEH probes, future msync / madvise /
// numa_ops consumers).** Anchors `BackingPinSlot::kReaderPin` once at
// the read site via `anchor_backing_reader_pin()`. There is no
// LockedSet; peer envelopes can run, kill backings, and recycle slots
// while the reader is mid-read. The anchor pin alone fixes the era
// but does not prevent inter-field staleness within the pinned scope
// (preempted between two field loads while peer killed-and-recycled).
// `BackingView`'s per-load generation re-check is the structural
// defence: every `view.load<&DescBacking::field>(mo)` reloads
// `generation` post-field-read and traps on mismatch.
//
// Crystalline-W's reservation primitive (`protect()`) is non-cumulative
// — each call on a (thread, slot index) pair supersedes the previous
// era reservation — and has no matching release; the slot is reclaimed
// by the next `protect()` on the same index, by `clear_all()`, or by
// thread exit. The anchor functions are therefore one-shot calls with
// no destructor; there is no acquire/release pair to wrap.
//
// `kEngineAnchor` and `kReaderPin` are statically disjoint, so engine
// and reader sides cannot rotate each other's pin via Crystalline's
// per-index non-cumulative semantics.
//
// Forward-compat note. Under cross-domain retire transitivity
// (CROSS_DOMAIN_PIN_SOLUTIONS.md Proposal 5) the reader anchor becomes
// a redundant era refresh — the skiplist pin would already cover the
// backing. The anchor call and the `BackingView` re-check both reduce
// to no-ops in that future world; both are removable transformations.
namespace BackingPinSlot {
inline constexpr uint32_t kEngineAnchor = 0;
inline constexpr uint32_t kReaderPin = 1;
static_assert(kEngineAnchor != kReaderPin,
              "engine anchor and reader pin must be distinct slot indices — "
              "Crystalline-W protect() is non-cumulative per (thread, index), "
              "so sharing one index would let either side rotate the other");
} // namespace BackingPinSlot

/// Engine-path anchor pin on `BackingPinSlot::kEngineAnchor`. Called
/// once at `run_envelope` entry; covers every per-op execute and reaper
/// raw deref through the entire retry loop. Sibling va_tracker
/// calls inside the envelope do not rotate this slot.
void anchor_backing_engine_pin();

/// Reader-path anchor pin on `BackingPinSlot::kReaderPin`. Called once
/// at a cross-domain reader site (fork serializer, VEH probe, msync /
/// madvise / numa_ops); the pinned scope extends until the next
/// `protect()` on the same slot in this thread (or `clear_all()`, or
/// thread exit).
///
/// A grep over `anchor_backing_reader_pin` enumerates every
/// cross-domain reader entry in the tree. Pair with `BackingView` for
/// field reads, since the pin alone does not catch inter-field
/// staleness within the pinned scope.
void anchor_backing_reader_pin();

/// Generation-checking view of a `DescBacking`. Defends against
/// kill+recycle that slips through between successive field reads.
///
/// Constructed from the raw pointer (typically the return of
/// `deref_backing_raw`); snapshots `generation` at construction.
/// Every `load<MemberPtr>(mo)` reloads `generation` after the field
/// load and `__builtin_trap`s on mismatch — a slot whose generation
/// has advanced between view construction and the load is a different
/// incarnation, and the trap fires before the caller acts on the
/// wrong-incarnation field value.
///
/// Use only on the reader path. Engine callers under
/// `anchor_backing_engine_pin` + `LockedSet` already exclude the race
/// the view defends; wrapping engine accesses would add cost without
/// safety. A null pointer in is null-on-load — `operator bool()`
/// reports the wrap.
class BackingView {
public:
    /// Construct over a raw `DescBacking *`. Snapshots `generation`
    /// with ACQUIRE so subsequent `load` re-checks pair with the
    /// alloc-side seed write. Null in -> null view; subsequent loads
    /// on a null view trap.
    LIBC_INLINE explicit BackingView(DescBacking *b) noexcept : b_(b) {
        pinned_gen_ =
            b == nullptr
                ? 0
                : b->generation.load(cpp::MemoryOrder::ACQUIRE);
    }

    /// Empty view, equivalent to `BackingView{nullptr}`.
    LIBC_INLINE BackingView() noexcept : b_(nullptr), pinned_gen_(0) {}

    [[nodiscard]] LIBC_INLINE explicit operator bool() const noexcept {
        return b_ != nullptr;
    }

    /// Identity comparison against a raw `DescBacking *` (typically
    /// from a sibling `deref_backing_raw` under the engine anchor).
    /// Identity test only; does not re-validate generation.
    [[nodiscard]] LIBC_INLINE bool operator==(const DescBacking *o) const
        noexcept {
        return b_ == o;
    }
    [[nodiscard]] LIBC_INLINE bool operator!=(const DescBacking *o) const
        noexcept {
        return b_ != o;
    }

    /// Load an atomic field of `DescBacking` under the view's
    /// generation snapshot; reloads `generation` post-load and
    /// `__builtin_trap`s on mismatch.
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

    /// Read a plain (non-atomic) field of `DescBacking` under the
    /// view's generation snapshot. Used for write-once fields whose
    /// cross-thread happens-before is supplied by the descriptor
    /// publish (`shape`, `placeholder_pages`, `cached_chunk_id`,
    /// `cached_slot_idx`).
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

/// Raw dereference. Triple-validates bounds, canary, and generation;
/// on any mismatch `__builtin_trap()`. Returns nullptr only when
/// `ref` is `kBackingRefNull`.
///
/// \pre Caller already holds a Crystalline pin on the backing domain
///      — `anchor_backing_engine_pin` for engine paths,
///      `anchor_backing_reader_pin` for reader paths. The two pin
///      indices are statically disjoint; either is sufficient for the
///      raw deref to be safe against slot recycle. The recycle defence
///      is the pin; inter-field staleness defence (reader path only)
///      is `BackingView`.
[[nodiscard]] DescBacking *deref_backing_raw(BackingRef ref);

//===----------------------------------------------------------------------===//
// Bootstrap and fork hooks
//===----------------------------------------------------------------------===//

/// One-shot domain registration. Called from `va_tracker_init_fn`
/// after the skiplist init.
void backing_init();

/// Fork reinitialization at priority `kForkPrioVaTracker` (39), and
/// from `pre_fork_drain`. Drops every pin on the backing domain
/// (`clear_all`), refreshes per-slot canaries against the rotated
/// `partition_secret`, and reclaims any pool slots whose pre-fork
/// retire batches were stranded by a dead thread's Crystalline cell.
void backing_fork_reinit();

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// BatchLinkCodec specialization
//===----------------------------------------------------------------------===//

// Required by every TU that instantiates
// `CrystallineDomain<DescBacking, ...>`. Encodes
// `(chunk_id << 8) | slot_idx` plus 1 as the 31-bit slot identifier
// the substrate's `batch_link` 4-byte field needs (the +1 keeps
// codec output 0 the substrate's unretired sentinel). Body lives in
// `desc_backing.cpp` because the decoder needs file-scope access to
// `g_backing_state.chunk_table[]` to recover the chunk_base from a
// chunk_id.

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
