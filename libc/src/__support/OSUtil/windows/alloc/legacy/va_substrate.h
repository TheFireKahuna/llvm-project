//===-- Unified placeholder-backed VA substrate ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// VaSubstrate is the shared VA provisioning layer for every internal
// allocator in the libc (SlabPool, IndexedPool, RadixStore, ThreadScratch).
// Consumers call `acquire(class)` to receive a hardened, placeholder-backed
// subslot handle; they return it with `release(handle)`. Subslot-level
// management below the substrate (freelists, xthread queues, page-state
// machines, etc.) stays inside each consumer; the substrate is only
// concerned with VA.
//
// Design goals (from plan `we-have-a-reusable-vast-valley.md`):
//
//   1. Self-host: every live arena is stamped once as a LIBC_INTERNAL
//      sentinel in the mapping table. Subslot churn is invisible to
//      the table; the population stays O(arenas), not O(allocations).
//
//   2. Demand-alloc / demand-free: each subslot is individually
//      placeholder-committed on acquire and placeholder-preserved on
//      release. Unused slots remain as PAGE_NOACCESS placeholders —
//      zero physical cost, implicit inter-subslot guard pages.
//
//   3. Lock-free hot path: one atomic RMW + one syscall per acquire/
//      release. No global locks. Slow paths use CAS-sentinel election.
//
//   4. Hardening beyond SlabPool:
//      - Per-arena authenticity tag (arena_tag = secret ^ arena_base ^ gen)
//      - Sealed secret in PCB Zone 0 (PAGE_READONLY after Tier A)
//      - Per-subslot canary + CAS-consumed owner-token
//      - Guard pages (leading + header + trailing, MMU-enforced)
//      - O(1) arena-membership bitmap (SubstrateRegistry) — every release
//        validates the pointer is inside a registered arena before any
//        derived dereference that could AV on a forged pointer.
//      - Full VA reclamation via epoch-gated MEM_RELEASE.
//
//   5. Zero new primitives: every hardening mechanism reuses an existing
//      `alloc/primitives/` component (AtomicBitmap, CanarySeed,
//      InitLatch) and the thread_registry epoch machinery.
//
// This header declares the public consumer-facing API and the internal
// arena/pool types. Implementation lives in va_substrate.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_VA_SUBSTRATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_VA_SUBSTRATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/primitives/canary_seed.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/alloc/legacy/substrate_registry.h"
// crystalline_domain.h carries the full CrystallineNode definition;
// crystalline_local_state.h only forward-declares it, which is
// insufficient for ArenaHeader's `: public concurrent::CrystallineNode`.
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_serial_table.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

// ============================================================================
// Size classes
// ============================================================================
//
// Five classes cover every current consumer. The arena size / slot size
// numbers are compile-time constants and are asserted against each other
// inside va_substrate.cpp (layout validation).

enum class SubSlotClass : uint8_t {
  Small = 0,   // 16 KB slots,  1 MB arena — small scratch, SlabRegistry L2/L3
  Medium = 1,  // 64 KB slots,  2 MB arena — SlabPool 64 KB slabs, RegionPool
  Large = 2,   // 128 KB slots, 2 MB arena — IndexedPool chunks, RadixStore L3
  XLarge = 3,  // 256 KB slots, 4 MB arena — SlabRegistry L1 on 57-bit VA,
               //                              ThreadRing 256 KB buffers
  Huge = 4,    // 1 MB slots,   8 MB arena — SlabPool 1 MB (L3-pool) slabs
  Count = 5,
};

// ============================================================================
// Consumer tag (provenance stamp on every arena)
// ============================================================================
//
// Stamped into ArenaHeader at arena creation time and threaded through
// `acquire()` so substrate can validate per-consumer affinity hints,
// surface diagnostics ("which consumer owns this arena"), and enforce
// cross-consumer isolation in the affinity-fast path.
//
// Every public acquire signature takes a ConsumerTag — there is no
// default. New consumers add a value here and pass it explicitly at
// every call site.
//
// `_Seed` is a private internal sentinel used only by `pre_init` for the
// per-class seed reservations; it is not for consumer use.
enum class ConsumerTag : uint8_t {
  // ThreadScratch (value 0) used to live here. It was removed when
  // ThreadScratch moved off the substrate to direct page_reserve /
  // page_free ownership of its per-thread arenas. The slot is left
  // unused (rather than renumbered) so any historical telemetry that
  // recorded a ConsumerTag value retains its historical meaning.
  _ReservedFormerThreadScratch = 0,
  SlabPool      = 1,
  SlabRegistry  = 2,
  RegionPool    = 3,
  IndexedPool   = 4,
  MappingTable  = 5,

  // Internal — pre_init's seed arena stamp. Permanent arenas only.
  _Seed         = 255,
};

// ============================================================================
// SubSlotHandle — ownership proof returned by acquire()
// ============================================================================
//
// Consumers treat this as opaque. Internals:
//   - `ptr` is the consumer-visible subslot base (always non-null when
//     the handle is truthy). The consumer may read/write
//     [ptr, ptr + slot_size_for_class).
//   - `token` is an unforgeable-without-Zone-0-secret binding between
//     `ptr` and the arena's current generation. `release()` verifies
//     and atomically consumes it; a forged or stale token traps.
//
// Token layout:
//   bits [63:61]  class_id (SubSlotClass — 3 bits, fits 8 classes; Count=5
//                 today, headroom for 3 more before re-pack)
//   bits [60: 0]  authenticity = low-61 bits of
//                 (substrate_token_key ^ slot_base ^ generation)
//
// Class bits live in the token so VaSubstrate::release can dispatch to
// the right pool without probing arena headers at wrong offsets — a
// wrong-class dereference would fault on placeholder memory. Three bits
// is minimal leakage; the 61 bits of XOR-mixed authenticity still make
// guessing a valid token infeasible without the Zone-0 key (2^61 ≈
// 2.3×10^18 attempts per key).
//
// Not default-constructible to a "valid" state: only VaSubstrate can
// mint a live handle. A default-constructed handle is safely empty —
// bool conversion returns false; passing it to release() traps.
//
// Copy + move are trivial and the consumer is free to hand the handle
// to another thread. Security still holds because the token's XOR
// components include the slot_base address, so a token is useful only
// against its originating slot.

class [[nodiscard]] SubSlotHandle {
public:
  LIBC_INLINE constexpr SubSlotHandle() = default;
  LIBC_INLINE constexpr SubSlotHandle(const SubSlotHandle &) = default;
  LIBC_INLINE constexpr SubSlotHandle(SubSlotHandle &&) = default;
  LIBC_INLINE constexpr SubSlotHandle &operator=(const SubSlotHandle &) =
      default;
  LIBC_INLINE constexpr SubSlotHandle &operator=(SubSlotHandle &&) = default;

  [[nodiscard]] LIBC_INLINE constexpr explicit operator bool() const {
    return ptr_ != nullptr;
  }
  [[nodiscard]] LIBC_INLINE constexpr void *ptr() const { return ptr_; }
  [[nodiscard]] LIBC_INLINE constexpr uint64_t token() const { return token_; }

  // Release the ptr half of the handle while leaving the token intact.
  // Used by consumers that hand off the raw pointer to code that cannot
  // carry the full handle, and preserve the token as a separate release
  // receipt. Pairs with `from_detached` below.
  [[nodiscard]] LIBC_INLINE constexpr void *detach_ptr() {
    void *p = ptr_;
    ptr_ = nullptr;
    return p;
  }

  // Reconstruct a handle from a detached `ptr` (returned by
  // `detach_ptr`) and the matching `token`. The two-argument private
  // constructor is reserved for substrate internals; this factory is
  // the explicit, audited path consumers use when they have to carry
  // the two halves separately. Subsequent `substrate_release(handle)`
  // performs the same authenticity checks as if the handle had never
  // been split.
  [[nodiscard]] LIBC_INLINE static constexpr SubSlotHandle
  from_detached(void *p, uint64_t t) {
    return SubSlotHandle(p, t);
  }

private:
  friend class VaSubstrate;
  friend class ArenaPool;

  LIBC_INLINE constexpr SubSlotHandle(void *p, uint64_t t)
      : ptr_(p), token_(t) {}

  void *ptr_{nullptr};
  uint64_t token_{0};
};

// Forward decls so ArenaPool can befriend VaSubstrate and vice versa.
class ArenaPool;
class VaSubstrate;

// ============================================================================
// Arena layout constants (per class)
// ============================================================================
//
// `SubSlotLayout<C>` is a template that exposes the per-class geometry
// as compile-time constants. Every runtime path that needs arena_size,
// slot_size, slots_per_arena, or the header offset goes through
// layout_of(class) (defined in va_substrate.cpp) which dispatches on
// the runtime enum and returns an inline struct populated from the
// template instantiation. Every constant is then a single load of a
// cache-line-local byte — no indirection.

struct SubSlotLayout {
  size_t arena_size;          // Total arena VA (bytes).
  size_t slot_size;            // Per-subslot size (bytes).
  size_t header_offset;        // Offset of ArenaHeader inside arena.
  size_t slot_region_offset;   // Offset of first subslot inside arena.
  unsigned slots_per_arena;    // Number of subslots.
  SubSlotClass klass;
};

// Retrieve the layout for a compile-time class enum.
[[nodiscard]] SubSlotLayout layout_of(SubSlotClass c);

// ============================================================================
// ArenaHeader — per-arena metadata (one cache-line aligned; lives in
// its own committed page inside the arena VA, guarded by pre/post guards).
// ============================================================================
//
// The header page is committed eagerly at arena creation; every byte of
// the slot region stays as placeholder until commit_slot() runs on
// acquire. Fields are ordered for cache-line locality of the hot
// mutation path (live_count RMW + occupancy RMW = one cache line each).

inline constexpr unsigned kMaxSlotsPerArena = 64;

// Arena layout constants shared across all classes. The guard/header/guard
// prefix and the trailing guard are uniform — only the slot region's
// length varies per class. Exposing these here lets `ArenaHeader` inline
// methods compute slot addresses without the per-class `layout_of()`
// switch on every release hot path.
inline constexpr size_t kGuardBytes = 4096;
inline constexpr size_t kHeaderPageBytes = 4096;
inline constexpr size_t kLeadingBytes =
    kGuardBytes + kHeaderPageBytes + kGuardBytes; // 12 KB
inline constexpr size_t kTrailingGuardBytes = kGuardBytes;

// Sentinel stored in `live_count` when the arena has been retired.
// Selected so `N → N+1` CAS from an in-flight acquire cannot accidentally
// land on the sentinel (UINT_MAX - 1 + 1 overflows, not == SENTINEL
// without an explicit transition from 0 via retire). The 0 → SENTINEL
// CAS is the single atomic retirement act; see try_retire_arena() in
// the .cpp. After the CAS the arena is handed to `g_substrate_domain.retire`
// — Crystalline drives reclamation; the substrate never touches the
// retired arena's storage again.
inline constexpr uint32_t kQuarantineSentinel = ~uint32_t{0};

// Shadow metadata stored in the header per subslot. Kept in the header
// (not inline in the subslot) so the consumer-visible slot memory is
// a pure buffer — a subslot-level UAF cannot corrupt the canary.
//
// Both fields are atomic: owner_token is CAS-consumed on release, and
// canary is written / zeroed / compared across threads. On x86_64
// aligned 8-byte loads/stores already have the access semantics we
// need, but the atomic wrappers document the contract and keep the
// type safe on weaker architectures.
struct SubSlotMeta {
  // canary = derive_canary(arena->arena_secret, slot_base, slot_size).
  // Verified on release; mismatch means a UAF write corrupted the
  // shadow state. Zeroed on release so a stale meta entry cannot be
  // replayed against a re-acquired slot.
  cpp::Atomic<uint64_t> canary{0};

  // owner_token = (class << 61) | (substrate_token_key ^ slot_base ^
  //                                 generation ^ acquire_seq) & low-61.
  // CAS-consumed on release: only one thread can win the CAS-to-zero
  // transition, so concurrent double-release races collapse to exactly
  // one successful release and (N-1) traps.
  cpp::Atomic<uint64_t> owner_token{0};

  // Per-slot monotonic acquire counter. Bumped on every claim of THIS
  // slot; folded into the token's authenticity hash. Defends against
  // same-slot replay: a buggy consumer that retains a previously-
  // released token cannot consume the slot's next acquisition because
  // every acquire mints a token from a fresh acquire_seq. Without this
  // field, generation is per-arena, not per-slot — every acquire of
  // the same slot in the same arena would mint the SAME token, and a
  // stored-then-replayed token would silently consume the new owner's
  // slot via the [R5] CAS.
  //
  // Single-writer per slot at the moment of claim (the bitmap-claim
  // CAS in `try_claim_free_slot` fences off all other claimants), so
  // a non-atomic uint32_t would suffice; cpp::Atomic + RELAXED
  // documents the cross-thread visibility contract and stays correct
  // if a future caller reads it from outside the claim path. uint32_t
  // gives 4 billion acquisitions per slot before wrap — practically
  // unbounded.
  cpp::Atomic<uint32_t> acquire_seq{0};
  uint32_t _pad0{0};
};

// ArenaHeader inherits the Crystalline intrusive-node layout. The five
// CrystallineNode words (next/slot/birth_era union, batch_link,
// refs/batch_next union) at the very front of the struct are owned by
// Crystalline once `g_substrate_domain.retire(this)` has been called;
// before retire they are zero (set by the memset in
// `initialize_arena_header`). For permanent arenas (seeds + the
// bootstrap-tier scratch synthetic header) the Crystalline fields stay
// zero forever — those arenas never enter retire and never appear in
// any retire batch.
struct alignas(64) ArenaHeader : public concurrent::CrystallineNode {
  // [0..19] Intrusive Crystalline-W runtime fields emitted directly so
  // ArenaHeader is standard-layout. `arena_serial` packs at the
  // natural 4-byte slot at offset 20 immediately after batch_link.
  LIBC_CRYSTALLINE_NODE_FIELDS(ArenaHeader);

  // Crystalline-W batch_link decode key. Stamped once at
  // `reserve_new_arena` from VaSubstrate's monotonic counter; immutable
  // for the arena's lifetime. Decoded via the per-substrate
  // `arena_serial_table` (see crystalline_serial_table.h). Permanent
  // arenas (never retired) leave this 0 — they never enter the codec.
  //
  // Placed at offset 20 of ArenaHeader by natural 4-byte alignment
  // following the macro's batch_link at offset 16 — standard-layout
  // declaration-order placement.
  uint32_t arena_serial{0};

  // --- Line 0+: immutable-after-init authenticity state ---
  //
  // All three values are set once when the arena is first created or
  // re-committed after epoch drain. The tag carries generation so a
  // stale pointer into retired-then-reused VA fails the tag check.
  uintptr_t arena_tag;    // substrate_secret ^ arena_base ^ generation
  uintptr_t arena_secret; // independent draw; keys the canary
  // Start of the arena's VA reservation. Stored explicitly (8 B) rather
  // than reconstructed via `this - (header_offset << k)` so the synthetic
  // bootstrap-tier header — whose enclosing reservation is not aligned to
  // its claimed class's `arena_size` — has an unconditionally-correct
  // arena base. Substrate-served arenas store the same value the old
  // header-offset trick would have computed.
  void *reservation_base;
  uint32_t generation;      // bumped on every arena recycle
  uint16_t class_id;        // value of SubSlotClass
  uint16_t slots_per_arena;
  uint32_t slot_size;       // bytes per subslot
  // Permanent arenas are never retired and never `page_free`'d by the
  // substrate. Two callers set this bit:
  //   1. `pre_init` on each per-class seed arena — the Small seed holds
  //      the mapping table's L1 directory; releasing it would kill the
  //      table mid-process.
  //   2. ThreadScratch's bootstrap-tier first-thread arena — its VA is
  //      owned by ThreadScratch (raw NT reservation), not the substrate;
  //      the substrate only tracks it via a synthetic ArenaHeader so
  //      introspection finds it on `pools_[Medium].all_head_`.
  // The flag short-circuits `try_retire_arena` and the `state == 0`
  // branch of `pop_abandoned_filtered` (drained-permanent arenas are
  // re-promotable as active, not retireable). `destroy` skips
  // `page_free` on permanent arenas, and `reserve_new_arena` skips
  // `init_node` so the Crystalline anchor fields stay zero.
  uint8_t is_permanent : 1;
  uint8_t _flags_reserved : 7;
  // Consumer provenance — stamped at reserve_new_arena. Values are
  // `static_cast<uint8_t>(ConsumerTag)`; 255 (`_Seed`) marks pre_init's
  // permanent seed arenas. Used by `acquire`'s affinity-hint validation
  // (cross-consumer affinity is rejected) and by diagnostics.
  uint8_t consumer_tag;
  uint8_t _pad0[2];

  // --- Hot mutable state ---
  //
  // live_count encodes the arena's lifecycle state:
  //   0 … slots_per_arena          → LIVE, count of allocated subslots
  //   kQuarantineSentinel          → RETIRED, no new acquires allowed
  //
  // Transition 0 → kQuarantineSentinel via CAS is the atomic retire act.
  // A concurrent acquire's N → N+1 CAS on the same word either wins
  // (retire aborts) or loses (retire proceeds). One RMW chain.
  cpp::Atomic<uint32_t> live_count;
  uint32_t _pad1_0;
  ArenaHeader *next_abandoned;        // Treiber link for abandoned stack
  ArenaHeader *all_next;              // doubly-linked list of all arenas
  ArenaHeader *all_prev;
  uint32_t _pad1_1[2];

  // --- Occupancy bitmap + shadow metadata ---
  internal::alloc_primitives::AtomicBitmap<kMaxSlotsPerArena,
                                           /*trap_on_collision=*/true>
      occupancy;

  SubSlotMeta slot_meta[kMaxSlotsPerArena];

  // Accessors — all constexpr-friendly reads.
  [[nodiscard]] LIBC_INLINE uintptr_t arena_base() const {
    return reinterpret_cast<uintptr_t>(reservation_base);
  }

  [[nodiscard]] LIBC_INLINE uintptr_t slot_region_base() const;
  [[nodiscard]] LIBC_INLINE void *slot_base_of(unsigned idx) const;
};

// Header fits inside its committed 4 KB page with slack — asserted in
// va_substrate.cpp so a future field addition can't silently outgrow
// the header-page budget.

// ============================================================================
// ArenaPool — one per SubSlotClass
// ============================================================================
//
// Manages the per-class arena set:
//   * active_        — currently feeding arena (hottest alloc target)
//   * abandoned_head_ — stack of non-empty arenas that stepped aside
//                       (a new active_ superseded them)
//
// Retired arenas leave the substrate immediately: `try_retire_arena`
// CASes `live_count` 0 → kQuarantineSentinel and hands the arena to
// `g_substrate_domain.retire(a)`. Crystalline owns the post-retire
// lifetime — slot publication, refcount accounting, and final
// `substrate_free_arena(a)` invocation when no thread holds a
// reservation. The substrate keeps no quarantine list of its own.
//
// New-arena election: a `reserving_` boolean elects exactly one
// thread to call `reserve_new_arena` per pool. Losers spin on the
// flag; the winner does the syscall, publishes into `active_`, and
// clears the flag. `active_` itself only ever holds nullptr or a
// real ArenaHeader* — that lets `g_substrate_domain.protect(active_,
// idx, nullptr)` dereference the loaded value safely without a
// non-canonical sentinel that would AV under Crystalline's slow_path.

class ArenaPool {
public:
  // VaSubstrate drives collect_seed_receipts / fork_reinit / destroy,
  // which need to walk the per-pool all_head_ list under all_lock_.
  friend class VaSubstrate;

  LIBC_INLINE ArenaPool() = default;
  ArenaPool(const ArenaPool &) = delete;
  ArenaPool &operator=(const ArenaPool &) = delete;
  ArenaPool(ArenaPool &&) = delete;
  ArenaPool &operator=(ArenaPool &&) = delete;

  // Called once by VaSubstrate::pre_init per class, before any acquire.
  // Record the class's layout constants on this pool. Does not reserve
  // any VA — the first acquire() triggers slow_acquire_arena which
  // reserves the first arena lazily. Runs during Phase 0a; does NOT
  // depend on any PCB accessor beyond what set_substrate_* populate in
  // the same window.
  void configure_class_only(SubSlotClass c);

  // Acquire a subslot from this pool. Returns an empty handle only if
  // VA cannot be reserved (process VA exhaustion — unrecoverable; the
  // caller traps by contract). The slot's pages are committed
  // (PAGE_READWRITE) at return — caller may read/write the full
  // [ptr, ptr + slot_size) range immediately.
  //
  // `tag` declares the consumer (SlabPool / SlabRegistry / etc.); stamped
  // into any newly-reserved arena. `affinity_hint`, if non-null, names
  // an arena to try first — substrate validates the hint and on success
  // serves a slot from it (improves locality across slab/chunk bursts).
  // On any hint-validation failure or hint exhaustion, substrate silently
  // falls through to the global active arena.
  [[nodiscard]] SubSlotHandle acquire(SubstrateRegistry &registry,
                                       ConsumerTag tag,
                                       ArenaHeader *affinity_hint);

  // Acquire a subslot whose VA is reserved but NOT committed. Caller is
  // responsible for `page_commit`-ing the subranges it needs and for
  // tolerating PAGE_NOACCESS on the rest. Used by consumers (e.g.
  // ThreadScratch) whose access pattern eagerly commits only a fraction
  // of the slot — saves the per-acquire `page_commit` syscall and the
  // physical RAM for never-touched pages. Release path is unchanged:
  // a single `page_decommit(slot_size)` at release covers whatever was
  // committed, regardless of how the caller carved up the range.
  [[nodiscard]] SubSlotHandle
  acquire_uncommitted(SubstrateRegistry &registry, ConsumerTag tag,
                      ArenaHeader *affinity_hint);

  // Release a previously-acquired subslot. Traps on any authenticity /
  // double-free / forged-token failure.
  void release(SubSlotHandle handle);

  // Fork reinit: single-threaded in the child. Clears per-thread hot
  // state (active_, abandoned_head_, reserving_). Crystalline-driven
  // retires from the parent are dropped (the domain's fork_reinit
  // already zeroed every surviving thread's slot/batch state — the
  // retire batches that referenced parent-side activity are gone). The
  // surviving arenas on `all_head_` retain their live_count / occupancy
  // / slot_meta via CoW; consumers re-wire their own view.
  void fork_reinit(SubstrateRegistry &registry);

  // Process-shutdown sweep. Releases every arena. Not lock-free; called
  // from the fini path where no concurrent readers exist.
  void destroy(SubstrateRegistry &registry);

  [[nodiscard]] LIBC_INLINE uint32_t arena_count() {
    return arena_count_.load(cpp::MemoryOrder::ACQUIRE);
  }

  [[nodiscard]] LIBC_INLINE const SubSlotLayout &layout() const {
    return layout_;
  }

private:
  // Shared body for `acquire` and `acquire_uncommitted`. The `commit_slot`
  // bool is a constant at every concrete call site — the compiler is
  // expected to fully specialize via inlining + branch elimination, so
  // the runtime cost is identical to two hand-written variants. `tag`
  // names the consumer (stamped into any newly-reserved arena);
  // `affinity_hint` is the optional arena-affinity hint (nullptr disables
  // the affinity fast path).
  [[nodiscard]] SubSlotHandle acquire_impl(SubstrateRegistry &registry,
                                            bool commit_slot, ConsumerTag tag,
                                            ArenaHeader *affinity_hint);

  // Per-arena slot claim helper — used by both the affinity fast path
  // and the active_-loop body. Performs [A2] live_count CAS-reserve, [A3]
  // bitmap claim, and [A4] meta stamp. Does NOT commit the slot. Returns
  // a non-null `slot_base` on success (caller commits if needed); a
  // nullptr `slot_base` indicates the arena was full/quarantined or lost
  // the bitmap-claim race — caller decides whether to demote/swap or
  // retry the same arena. On commit failure the caller calls
  // `claim_rollback` to undo [A2]/[A3]/[A4] in reverse order.
  struct SlotClaim {
    void *slot_base;  // nullptr on failure
    uint64_t token;
    unsigned idx;
  };
  [[nodiscard]] SlotClaim claim_slot_in_arena(ArenaHeader *a);
  void claim_rollback(ArenaHeader *a, unsigned idx);

  // Slow-path helpers — defined in va_substrate.cpp.
  [[nodiscard]] ArenaHeader *slow_acquire_arena(SubstrateRegistry &registry,
                                                  ConsumerTag tag);
  [[nodiscard]] ArenaHeader *pop_abandoned_filtered();
  void install_as_active(ArenaHeader *a);
  void push_abandoned(ArenaHeader *a);
  // Push an already-linked chain (interior next_abandoned links pre-set)
  // onto abandoned_head_ in one CAS-loop. Used by the consumer side of
  // pop_abandoned_filtered to re-publish the unexamined remainder + the
  // deferred-full set in a single atomic publish.
  void push_abandoned_chain(ArenaHeader *head);
  // `permanent`: if true, the arena's `is_permanent` flag is set BEFORE
  // any Crystalline init_node call. Used by pre_init for the per-class
  // seed arenas — they must never enter Crystalline retire and the
  // is_permanent=1 stamp lets the function skip init_node entirely
  // (avoiding a my_thread()-via-get_thread_scratch recursion that would
  // trigger ThreadScratch's create_thread_state during substrate
  // bring-up, which would in turn enqueue a bootstrap receipt for an
  // arena synth that collect_seed_receipts would then emit a
  // duplicate, mis-sized receipt for).
  [[nodiscard]] ArenaHeader *reserve_new_arena(SubstrateRegistry &registry,
                                                ConsumerTag tag,
                                                bool permanent = false);
  void initialize_arena_header(ArenaHeader *hdr, uintptr_t base,
                               uint32_t generation, ConsumerTag tag);
  void try_retire_arena(ArenaHeader *a);

  void all_insert(ArenaHeader *a);
  void all_remove(ArenaHeader *a);

  // --- State --------------------------------------------------------------
  //
  // active_ on its own cache line: acquire hot path reads it every call.
  // Only ever holds nullptr or a real ArenaHeader* — Crystalline's
  // `read(active_, idx, ...)` dereferences the loaded pointer in its
  // slow_path, so a non-canonical sentinel would AV.
  alignas(64) cpp::Atomic<ArenaHeader *> active_{nullptr};

  // `reserving_` elects exactly one thread to perform a fresh-arena
  // reservation (NtAllocateVirtualMemoryEx) when active_ is nullptr and
  // abandoned_head_ holds no candidates. Election is a `false → true`
  // CAS; losers spin on the flag until the winner clears it. Replaces
  // the old non-canonical sentinel-in-active_ scheme — Crystalline's
  // slow_path dereferences the loaded pointer, so a non-pointer
  // sentinel value in `active_` would AV.
  cpp::Atomic<bool> reserving_{false};

  alignas(64) cpp::Atomic<ArenaHeader *> abandoned_head_{nullptr};
  cpp::Atomic<uint32_t> arena_count_{0};

  // Cold path: every-arena linked list for fork_reinit/destroy walks.
  // Protected by all_lock_; never touched on the hot path.
  alignas(64) ArenaHeader *all_head_{nullptr};
  cpp::Atomic<int> all_lock_{0};

  // Cached per-class layout. Populated by configure_class_only.
  SubSlotLayout layout_{};
};

// ============================================================================
// VaSubstrate — top-level orchestrator (one per process, namespace-scope)
// ============================================================================
//
// Lifecycle phases (all single-threaded by contract):
//
//   pre_init()              Invoked by the VaSubstrate `.libcmem` handler
//                           at Tier A Phase 0c.5 Pass 1. Seeds Zone 0
//                           secrets into PCB, reserves one seed arena per
//                           class. Arenas are NOT yet registered in the
//                           mapping table (table's own handler may not
//                           have run yet; registration lives in Pass 2).
//
//   collect_seed_receipts() Invoked by the same handler immediately
//                           after pre_init. Emits one Receipt per seed
//                           arena into the walker's buffer; the walker's
//                           Pass 2 batch-registers every Receipt as
//                           LIBC_INTERNAL in the now-live mapping table.
//                           Does NOT latch g_mapping_table_ready — that
//                           happens at the very end of the walker's
//                           Pass 2, after every receipt has been stamped
//                           and the mapping table is fully INIT_READY.
//
//   fork_reinit()           Post-RtlCloneUserProcess, single-threaded
//                           child. Clears per-pool hot state; arenas
//                           survive CoW.
//
//   destroy()               Process fini. Releases every arena.

class VaSubstrate {
public:
  LIBC_INLINE VaSubstrate() = default;
  VaSubstrate(const VaSubstrate &) = delete;
  VaSubstrate &operator=(const VaSubstrate &) = delete;
  VaSubstrate(VaSubstrate &&) = delete;
  VaSubstrate &operator=(VaSubstrate &&) = delete;

  // --- Lifecycle ---
  void pre_init();
  // Emit a Receipt for every seed arena reserved by pre_init() into the
  // walker's buffer. Called exactly once from the substrate's `.libcmem`
  // handler during Tier A Phase 0c.5. The walker's Pass 2 stamps every
  // receipt as LIBC_INTERNAL; only after that loop completes does the
  // walker latch `g_mapping_table_ready` (see
  // `memory_primitives_bootstrap.h`'s `mark_mapping_table_ready`). Until
  // that latch flips, every consumer that reserves VA during Tier A
  // (substrate seeds, ThreadScratch's bootstrap-tier first thread) must
  // defer registration via the Receipt mechanism — inline stamping
  // would reenter `ensure_init` on the same thread and self-deadlock.
  // Returns the number of receipts written (one per class: three today).
  uint32_t collect_seed_receipts(::LIBC_NAMESPACE::internal::Receipt *out,
                                 uint32_t cap);
  void fork_reinit();
  void destroy();

  // --- Hot path ---
  //
  // Every public acquire takes a ConsumerTag — there is no default. Add
  // a value to `ConsumerTag` and pass it explicitly at every call site.
  // The `affinity_hint` overload accepts an `ArenaHeader *` derived from
  // the consumer's prior allocation (e.g., `arena_of(prev_slot, c)`);
  // substrate validates the hint and on success serves a slot from it.
  // On any hint-validation failure or hint exhaustion, substrate silently
  // falls through to the global active arena. The non-affinity overload
  // is equivalent to passing nullptr but avoids any hint-validation cost.
  [[nodiscard]] SubSlotHandle acquire(SubSlotClass c, ConsumerTag tag);
  [[nodiscard]] SubSlotHandle acquire(SubSlotClass c, ConsumerTag tag,
                                       ArenaHeader *affinity_hint);
  // Acquire a slot whose VA is reserved but NOT committed. See
  // ArenaPool::acquire_uncommitted for rationale and contract.
  [[nodiscard]] SubSlotHandle acquire_uncommitted(SubSlotClass c,
                                                   ConsumerTag tag);
  [[nodiscard]] SubSlotHandle acquire_uncommitted(SubSlotClass c,
                                                   ConsumerTag tag,
                                                   ArenaHeader *affinity_hint);
  void release(SubSlotHandle handle);

  // Drop every Crystalline reservation the calling thread holds on the
  // substrate domain. Equivalent to `g_substrate_domain.clear_all()` —
  // exposed here as a substrate-level entry point so callers do not
  // need to reach into the Crystalline template directly.
  //
  // Reclamation under Crystalline is asynchronous and automatic; this
  // call is NOT required for correctness. It exists for two narrow
  // purposes:
  //   1. Test/diagnostic surface — drive deterministic drain after
  //      releasing many handles in a single-threaded workload.
  //   2. Long-lived API boundaries — a thread that has not entered
  //      a substrate-allocating path for a long time may leave a stale
  //      slot 0 reservation pinning the most-recent active arena. A
  //      shutdown or quiescence path can call this to give Crystalline
  //      a chance to reap.
  void try_reclaim();

  // --- Introspection ---
  [[nodiscard]] LIBC_INLINE uint32_t arena_count(SubSlotClass c) {
    return pools_[static_cast<unsigned>(c)].arena_count();
  }

  // Header-inline so constexpr use sites (e.g. mapping_table's
  // ensure_init assert) can evaluate without a redeclaration. The
  // constants mirror ClassLayout<C>::kSlotSize in va_substrate.cpp;
  // a static_assert there pins the pairing.
  [[nodiscard]] static constexpr size_t slot_size(SubSlotClass c) {
    return c == SubSlotClass::Small    ? (size_t{16} << 10)
           : c == SubSlotClass::Medium ? (size_t{64} << 10)
           : c == SubSlotClass::Large  ? (size_t{128} << 10)
           : c == SubSlotClass::XLarge ? (size_t{256} << 10)
                                       : (size_t{1} << 20);
  }

  // Per-class arena size — power-of-2; arena_base = ptr & ~(arena_size-1).
  // Used by `is_substrate_owned`, `arena_of`, and consumers that need to
  // round a slot pointer down to its owning arena base. Mirrors the
  // ClassLayout<C>::kArenaSize constants in va_substrate.cpp; a static
  // assert there pins the pairing.
  [[nodiscard]] static constexpr size_t arena_size(SubSlotClass c) {
    return c == SubSlotClass::Small    ? (size_t{1} << 20)
           : c == SubSlotClass::Medium ? (size_t{2} << 20)
           : c == SubSlotClass::Large  ? (size_t{2} << 20)
           : c == SubSlotClass::XLarge ? (size_t{4} << 20)
                                       : (size_t{8} << 20);
  }

  // Exposed so consumers that want to batch many sub-operations against a
  // single SubstrateRegistry::contains check can do so (e.g. the mapping
  // table's internal-region audit). Not for general use.
  [[nodiscard]] LIBC_INLINE SubstrateRegistry &registry() { return registry_; }

  // Pool-level teardown helper. Called from `substrate_free_arena` (the
  // FreeFn handed to `g_substrate_domain`) once Crystalline has confirmed
  // no thread holds a reservation on the arena. Drops the arena off the
  // owning pool's all_head_ list and decrements arena_count_. The
  // mapping-table remove / registry remove_range / page_free are
  // performed by `substrate_free_arena` itself; this is the bridge that
  // keeps the arena pool's bookkeeping reachable without making
  // substrate_free_arena a friend of `ArenaPool`.
  void detach_arena_from_pool(SubSlotClass c, ArenaHeader *a);

  // Allocate a process-globally-unique arena generation value. Bumped
  // on every reserve_new_arena call; the arena's authenticity tag folds
  // `generation` into `arena_secret ^ arena_base`, so a stale handle
  // into a retired arena cannot forge a valid tag against a fresh arena
  // that happened to reuse the same base VA.
  //
  // Not in Zone 0b: forging an arena_tag requires knowing
  // substrate_secret (sealed in Zone 0, PAGE_READONLY for life). The
  // generation is a freshness counter; an arbitrary-write attacker who
  // rewrites it still cannot produce `secret ^ base ^ gen` matching a
  // stored tag without the secret. Keeping it outside Zone 0b avoids a
  // two-syscall unseal/reseal cycle on every arena reservation.
  [[nodiscard]] LIBC_INLINE uint32_t next_generation() {
    return next_generation_.fetch_add(1, cpp::MemoryOrder::RELAXED) + 1;
  }

  // Allocate a process-globally-unique arena serial for the
  // Crystalline-W batch_link codec (see ArenaHeader::arena_serial). The
  // returned value is monotonic, non-zero, and stays under 2^31 by
  // construction (overflow is unreachable under any realistic workload
  // — the serial table traps on insert beyond capacity). Independent
  // from `generation`: generation may rotate per arena recycle, while
  // the serial is fixed-once at first allocation and persists in the
  // serial_table for decode.
  [[nodiscard]] LIBC_INLINE uint32_t next_arena_serial() {
    return arena_serial_counter_.fetch_add(1, cpp::MemoryOrder::RELAXED) + 1;
  }

  // Publish an ArenaHeader into the per-substrate serial table so the
  // Crystalline-W codec can decode `arena_serial → ArenaHeader *`.
  LIBC_INLINE void publish_arena_serial(uint32_t serial, ArenaHeader *hdr) {
    arena_serial_table_.insert(serial, hdr);
  }

  // Resolve a serial back to its ArenaHeader. Used by the codec's
  // decode path. Returns nullptr if the serial has never been
  // published (codec contract violation).
  [[nodiscard]] LIBC_INLINE ArenaHeader *
  arena_by_serial(uint32_t serial) const {
    return arena_serial_table_.lookup(serial);
  }

private:
  friend class ArenaPool;

  SubstrateRegistry registry_;
  ArenaPool pools_[static_cast<unsigned>(SubSlotClass::Count)];
  internal::alloc_primitives::InitLatch init_latch_;
  // Starts at 0; first `next_generation()` returns 1. Zero is reserved
  // as the "never seeded" sentinel — if a consumer ever observes an
  // arena_tag whose generation component is zero after Tier A, the
  // arena header failed to initialise and release() will trap on the
  // tag mismatch.
  cpp::Atomic<uint32_t> next_generation_{0};
  // Starts at 0; first `next_arena_serial()` returns 1. Crystalline-W
  // batch_link codec requires non-zero serials.
  cpp::Atomic<uint32_t> arena_serial_counter_{0};
  // Serial → ArenaHeader* lookup table. Each non-permanent arena
  // publishes itself here at `reserve_new_arena` time.
  concurrent::CrystallineSerialTable<ArenaHeader> arena_serial_table_;
};

// Namespace-scope global. Published into PCB Zone 0's `substrate_root_`
// by pre_init(); after the Tier A seal the pointer is hardware-immutable.
extern VaSubstrate g_substrate;

// Convenience free-function wrappers. Every consumer should call these
// rather than reaching into g_substrate directly — matches the pattern
// used by SlabPool::alloc() / SlabPool::free() at call sites. Two
// overloads each: the basic form picks the global active arena; the
// `affinity_hint` form prefers a caller-supplied arena and falls
// through to active on validation failure.
[[nodiscard]] LIBC_INLINE SubSlotHandle substrate_acquire(SubSlotClass c,
                                                            ConsumerTag tag) {
  return g_substrate.acquire(c, tag);
}
[[nodiscard]] LIBC_INLINE SubSlotHandle substrate_acquire(
    SubSlotClass c, ConsumerTag tag, ArenaHeader *affinity_hint) {
  return g_substrate.acquire(c, tag, affinity_hint);
}
// Reserve a slot's VA without committing physical pages. See
// ArenaPool::acquire_uncommitted for the contract.
[[nodiscard]] LIBC_INLINE SubSlotHandle
substrate_acquire_uncommitted(SubSlotClass c, ConsumerTag tag) {
  return g_substrate.acquire_uncommitted(c, tag);
}
[[nodiscard]] LIBC_INLINE SubSlotHandle substrate_acquire_uncommitted(
    SubSlotClass c, ConsumerTag tag, ArenaHeader *affinity_hint) {
  return g_substrate.acquire_uncommitted(c, tag, affinity_hint);
}
LIBC_INLINE void substrate_release(SubSlotHandle handle) {
  g_substrate.release(handle);
}

} // namespace alloc
} // namespace windows

namespace concurrent {
// BatchLinkCodec for ArenaHeader — uses the stamped `arena_serial`
// + the substrate's per-process serial table for O(1) decode.
// Encoded value is `1 + arena_serial` (serial is monotonic, never 0
// after `next_arena_serial()` returns). Bit 31 is unreachable in
// practice (counter would have to bump past 2^31, far past the table
// capacity which traps first). In header so any TU using
// `g_substrate_domain` instantiates the codec.
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::alloc::ArenaHeader> {
  using Node = ::LIBC_NAMESPACE::windows::alloc::ArenaHeader;
  LIBC_INLINE static uint32_t encode(CrystallineNode *n) noexcept {
    return 1u + static_cast<Node *>(n)->arena_serial;
  }
  LIBC_INLINE static CrystallineNode *decode(uint32_t code) noexcept {
    return ::LIBC_NAMESPACE::windows::alloc::g_substrate.arena_by_serial(
        code - 1u);
  }
};
} // namespace concurrent

namespace windows {
namespace alloc {

// ============================================================================
// Sub-slot commit helpers + ownership predicate
// ============================================================================
//
// Consumers using `substrate_acquire_uncommitted` carve their slot into
// committed and uncommitted subranges (e.g., SlabPool's 4 KB header +
// 60 KB body + guard pages inside a 64 KB Medium slot). These wrappers
// bound-check the subrange against the slot size implied by the
// handle's class bits, then delegate to the page-level primitive.
//
// `substrate_commit_subrange` returns false on commit failure (rare — OOM
// or paging). `substrate_decommit_subrange` is best-effort and never
// returns failure (decommit on already-uncommitted range is idempotent).
// Both trap on out-of-bounds or empty `length`.
[[nodiscard]] bool substrate_commit_subrange(SubSlotHandle h, size_t offset,
                                              size_t length);
void substrate_decommit_subrange(SubSlotHandle h, size_t offset, size_t length);

// Predicate: returns true iff `p` falls inside any substrate-owned arena.
// Walks the SubSlotClass values, masking `p` by each class's arena_size
// and probing SubstrateRegistry::contains. Diagnostics-only — not for
// hot paths (worst case `Count` masked loads + bitmap probes).
[[nodiscard]] bool is_substrate_owned(const void *p);

// Round a slot pointer down to its owning ArenaHeader. Used by consumers
// constructing the `affinity_hint` argument to `substrate_acquire` from
// a pointer they already hold (their previous slot from this class).
//
// Returns the ArenaHeader at `(p & ~(arena_size-1)) + header_offset`.
// Does NOT validate that `p` is substrate-owned — consumers ensuring
// that should call `is_substrate_owned` first. The hint is then
// re-validated inside `substrate_acquire` (consumer_tag / class_id /
// live_count checks), so a stale or wrong hint is safely rejected.
[[nodiscard]] ArenaHeader *arena_of(const void *slot_ptr, SubSlotClass c);

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_VA_SUBSTRATE_H
