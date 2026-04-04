//===--- Lock-free wait slot pool for Treiber futex -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One pool slot per waiting thread, TLS-allocated on first use; the
// pool is a static array (zero dynamic allocation). Freelist is a
// Treiber stack of slot indices. Slot 0 is the null sentinel.
//
// Nesting: VEH handlers firing during a user-mode wait phase get a
// secondary slot via alloc_secondary(). Bounded to one nest level.
//
// slot.link is a `cpp::Atomic<linkage::Link>` — a 64-bit atomic
// packed as [state:8 | reserved:8 | tag:32 | next:16]:
//   next  — forward pool index.
//   tag   — monotonic per-link counter, bumped on every link write.
//           Closes in-place ABA on pred.next cycles.
//   state — SlotState. Folded into the link so the Harris walker's
//           mid-splice CAS natively fails if pred is concurrently
//           popped (pop's state CAS is itself a write to link).
//
// The Link type plus the link_cas_* family (state-machine-aware
// CAS, mark/finalize, certify, exchange) all live in the substrate
// at `<src/__support/OSUtil/windows/concurrent/lock_free_linkage.h>`.
// This file consumes those primitives via `WaitSlotStateTraits`,
// which encodes wait_slot's specific transition validation and
// alert-fire policy.
//
// state == TIMED_OUT is the logical-deletion marker — Treiber only
// inserts at head, so classical Harris's mid-chain-insert mark bit
// is unnecessary. Walkers read state as part of the link word; dead
// nodes get opp-spliced. Timeout path is a single link CAS.
//
// Reclamation: direct-push under the state+tag fold. reclaim_slot
// transitions slot.link to IDLE+CERT (atomic), bumps generation,
// clears subsystem, and pushes onto the freelist immediately. No
// hazard window or retire queue: the Safety Triad documented in
// `lock_free_linkage.h` (tag monotonicity + IDLE-on-reachable-chain
// retry + never-freed pool memory) makes any stale walker snapshot
// fail its mid-splice CAS, and the pool array is statically
// reserved (never freed) so stale dereferences are always to valid
// memory. Single-actor per lifecycle gen — Harris walker and
// parking-lot unlinker return true iff caller should reclaim_slot;
// false ⇒ slot handled elsewhere, caller MUST NOT touch it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_WAIT_SLOT_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_WAIT_SLOT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/lock_free_linkage.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// Forward decl — full definition in thread_lifecycle.h, which would
// cycle if included here (thread_lifecycle.h → futex_utils.h →
// wait_slot.h). The ThreadHandle struct itself is also defined
// there; to keep wait_slot.h pulled-in by the futex hot path
// light, the owner identity is stored as two raw uint32_t fields
// on the slot and callers reconstruct a ThreadHandle in
// wait_slot.cpp where the full header is included.
struct ThreadLifecycle;

namespace wait_slot {

// Slot states (slot.link.state — one atomic carrying stack-linkage
// lifecycle AND wake signal + wake kind + cleanup responsibility):
//
//   IDLE → WAITING ┬→ IN_KERNEL ┬→ SIGNALED_CLEAN             → IDLE
//                  │            ├→ SIGNALED_ORPHAN            → IDLE
//                  │            └→ TIMED_OUT                  → IDLE
//                  ├→ SIGNALED_CLEAN                          → IDLE
//                  ├→ SIGNALED_ORPHAN                         → IDLE
//                  ├→ SIGNALED_HANDOFF_CLEAN                  → IDLE
//                  ├→ SIGNALED_HANDOFF_ORPHAN                 → IDLE
//                  └→ TIMED_OUT                               → IDLE
//
// One link_cas_snap atomically transitions WAITING/IN_KERNEL →
// SIGNALED_* with tag bump — no separate wake_word.
//
// CLEAN   waker's detach CAS won; slot off-stack.
// ORPHAN  detach lost to an interleaved push; slot still linked,
//         waiter must self-splice before clear_slot_owned. Waker
//         may best-effort upgrade ORPHAN → CLEAN post-detach
//         (race with waiter's decode is benign — upgrade saves a
//         walk).
// HANDOFF ownership transferred; waiter acquires without re-CASing
//         value_. Emitted only by handoff_one's WAITING branch
//         (IN_KERNEL → HANDOFF would risk alert loss).
//
// TIMED_OUT slots stay physically linked until a walker splices
// them out (harris_unlink, pop_and_signal_one, drain_waiters, or
// Phase 1.75 self-reclaim). state == TIMED_OUT IS the logical-
// deletion marker; no separate mark bit needed.
//
// Encoding chosen so byte-7 spin_on_link_state exits on any state
// != WAITING — every SIGNALED_* differs, so Phase 2.5 terminates
// on any wake.
enum SlotState : uint8_t {
  IDLE = 0,
  WAITING = 1,   // Pushed onto Treiber stack, not yet in kernel.
  IN_KERNEL = 2, // Blocked in NtWaitForAlertByThreadId.
  SIGNALED_CLEAN = 3,  // Waker pre-marked AND detached; slot off-stack.
  TIMED_OUT = 4, // Timeout / thread-exit / EINTR — dead entry awaiting unlink.
  SIGNALED_ORPHAN = 5, // Waker pre-marked; detach CAS lost to a push;
                       // slot still linked — waiter must self-splice.
  SIGNALED_HANDOFF_CLEAN = 6,  // CLEAN + ownership transferred (HANDOFF_BIT).
  SIGNALED_HANDOFF_ORPHAN = 7, // ORPHAN + ownership transferred.
};

// Decoders for the SIGNALED_* family. Safe on any state byte —
// non-SIGNALED states return false.
LIBC_INLINE constexpr bool state_is_signaled(uint8_t st) {
  return st == SIGNALED_CLEAN || st == SIGNALED_ORPHAN ||
         st == SIGNALED_HANDOFF_CLEAN || st == SIGNALED_HANDOFF_ORPHAN;
}

LIBC_INLINE constexpr bool state_is_orphan(uint8_t st) {
  return st == SIGNALED_ORPHAN || st == SIGNALED_HANDOFF_ORPHAN;
}

LIBC_INLINE constexpr bool state_has_handoff(uint8_t st) {
  return st == SIGNALED_HANDOFF_CLEAN || st == SIGNALED_HANDOFF_ORPHAN;
}

// Validity table for SlotState transitions. Used by static_assert
// in `WaitSlotStateTraits::is_valid` — catches a typo that would
// silently succeed if byte values happen to match.
//
// Legal:
//   IDLE → WAITING                                  (Phase 2 entry)
//   WAITING → IN_KERNEL                             (Phase 3 commit)
//   WAITING/IN_KERNEL → SIGNALED_*                  (waker pre-mark)
//   WAITING/IN_KERNEL → TIMED_OUT                   (owner self-cancel)
//   SIGNALED_ORPHAN → SIGNALED_CLEAN                (waker upgrade)
//   SIGNALED_HANDOFF_ORPHAN → SIGNALED_HANDOFF_CLEAN
//   <non-IDLE> → IDLE                               (clear/reclaim)
//
// Self-transitions (X → X) are NOT legal — same-state CAS means
// the caller misread the protocol.
//
// Re-arm SIGNALED_*_CLEAN → WAITING (inkernel_repark) routes
// through linkage::link_store_state, an unconditional single-
// writer path that bypasses this table by design (re-arm is
// owner-exclusive).
LIBC_INLINE constexpr bool is_valid_state_transition(uint8_t from, uint8_t to) {
  // Catch-all: reclaim path can take any non-IDLE state to IDLE.
  if (to == IDLE)
    return from != IDLE;
  // Phase 2 entry.
  if (from == IDLE && to == WAITING)
    return true;
  // Phase 3 commit.
  if (from == WAITING && to == IN_KERNEL)
    return true;
  // Waker pre-mark — any of the four SIGNALED_* destinations from
  // a live state.
  if ((from == WAITING || from == IN_KERNEL) && state_is_signaled(to))
    return true;
  // Owner self-cancel.
  if ((from == WAITING || from == IN_KERNEL) && to == TIMED_OUT)
    return true;
  // Waker upgrade ORPHAN → CLEAN (within same handoff variant).
  if (from == SIGNALED_ORPHAN && to == SIGNALED_CLEAN)
    return true;
  if (from == SIGNALED_HANDOFF_ORPHAN && to == SIGNALED_HANDOFF_CLEAN)
    return true;
  return false;
}

// Wait-slot's binding policy for the substrate's state-aware CAS
// helpers (see `lock_free_linkage.h`).
//
//   is_valid:    delegates to is_valid_state_transition above.
//   fires_alert: pre-mark from IN_KERNEL into any SIGNALED_*
//                publishes LINK_ALERT_FIRED_BIT atomically with
//                the state transition. The waker fires an
//                unconditional alert against the IN_KERNEL owner;
//                the bit lets the waiter's SIGNALED-observation
//                site classify "alert in flight" structurally
//                (replaces the per-thread futex_wake_epoch
//                counter).
//
//                Pre-marks from WAITING leave ALERT_FIRED at 0 by
//                Phase 2 push commit invariant — Phase 2.5 cache
//                spin catches the wake without a syscall, so no
//                alert is ever issued.
//
// Both hooks are `static constexpr` so the substrate's
// `static_assert` and `if constexpr` evaluate at compile time.
struct WaitSlotStateTraits {
  static constexpr bool is_valid(uint8_t from, uint8_t to) {
    return is_valid_state_transition(from, to);
  }
  static constexpr bool fires_alert(uint8_t from, uint8_t to) {
    return from == IN_KERNEL && state_is_signaled(to);
  }
};

// Subsystem tag: which wait primitive owns the slot's link. Thread-
// exit cleanup dispatches on this — calling Futex::harris_unlink
// with a user address from the parking-lot path would walk
// arbitrary memory.
//
// `enum class` is structural: implicit conversion to/from integral
// is forbidden, so accidental `subsys + 1` or `== 0` is a compile
// error. Storage is uint8_t — cpp::Atomic<SubsystemKind> is
// layout-identical to the prior cpp::Atomic<uint8_t>.
enum class SubsystemKind : uint8_t {
  None = 0,       // Free / unused / between waits.
  Futex = 1,      // Linked in a Futex Treiber stack.
  ParkingLot = 2, // Linked in a futex_addr bucket SLL.
};

// Unified predicate for Futex::wait_on_predicate. The waiter
// installs it on slot.filter_fn at Phase 2 setup. Two roles:
//
//   Waiter-side stop condition  checked in wait_impl fast path,
//     Phase 1 hw spin, Phase 2 CAS-push, Phase 4 stale-alert + re-park,
//     top-level absorb. ret==0 guarantees pred(load(), arg) held at
//     some point in the wait.
//
//   Waker-side filter  pop_and_signal_one / signal_first_match_after
//     read filter_fn concurrently with the lock holder and skip
//     waiters whose pred is FALSE on current value_.
//
// CALLER NOTIFY CONTRACT (LOAD-BEARING): every FALSE→TRUE flip of
// pred(value_, arg) MUST be paired with notify_one/notify_all/
// unlock_notify on this futex. No eventual re-check fallback —
// missing the notify strands parked waiters.
//
// I7 contract:
//   - fn: nullptr (unconditional wake) or freestanding function with
//     process-lifetime validity. Waker reads concurrently with slot
//     reuse, so the pointer itself must remain dereferenceable
//     regardless of slot state — no captures, no member fns.
//   - arg: plain u32, NOT a pointer. The value-type restriction is
//     what makes torn reads of (fn, arg) during a slot-reuse cycle
//     tolerable: any garbage pair yields a deterministic bool, and
//     the waker's subsequent pre-mark CAS fails on the stale snap
//     so no corruption escapes.
//   - fn must be noexcept, side-effect-free, deterministic in its
//     two inputs. NO locks, allocation, or recursion into any wait
//     primitive — the waker invokes on the lock holder's critical
//     path; any blocking is a deadlock.
//
// noexcept is part of the type — non-noexcept fn triggers a
// compile error at the wait_on_predicate registration site.
using PredicateFn = bool (*)(uint32_t current_value, uint32_t arg) noexcept;

struct alignas(64) WaitSlot {
  // Which subsystem owns the link (set on link, cleared on
  // unlink/reclaim). Thread-exit cleanup dispatches on this.
  cpp::Atomic<SubsystemKind> subsystem{SubsystemKind::None};
  uint8_t _pad0[3]{};
  // Owning thread's NT TID. Read RELAXED concurrently with
  // freelist_push's zero-on-free / alloc_slot's rewrite-on-alloc;
  // the atomic annotation closes the formal C++ data race without
  // changing codegen (aligned 32-bit stores are machine-atomic).
  cpp::Atomic<uint32_t> thread_id{0};
  // The single wake-signal carrier — see SlotState above and the
  // Link doc in lock_free_linkage.h. Tag monotonicity is enforced
  // by Link's withers; the typed API has no path that writes a
  // Link without bumping tag.
  cpp::Atomic<linkage::Link> link{linkage::Link::pack(IDLE, 0, 0)};
  // Bumped on every alloc and reclaim — slot-lifecycle identity
  // for stale-TLS-ref detection. 32-bit so wraparound is
  // astronomical: at 10K reclaims/sec, >130 years. (16-bit would
  // wrap in <7 s under stress and break the gen-match guarantee.)
  // Atomic because the owner reads it in get_slot_index while
  // wakers / Harris walkers / thread-exit may concurrently
  // increment in reclaim_slot.
  cpp::Atomic<uint32_t> generation{0};
  uint32_t _pad1{0};
  // Wait target. Interpret via subsystem: SubsystemKind::Futex ⇒
  // Futex*; SubsystemKind::ParkingLot ⇒ user address. Used by
  // pop/handoff for stale-top dispatch and by harris_unlink to
  // find the Futex a dead slot needs evicting from.
  //
  // RELAXED everywhere — aligned uintptr_t loads/stores are
  // machine-atomic on x86/AArch64; the Atomic<> wrapper just
  // closes the formal C++ data race so TSan can validate.
  cpp::Atomic<uintptr_t> wait_address{0};

  // Captured ThreadHandle of the slot's allocating thread — used
  // by wakers to validate the slot's original owner is still
  // registered with the same identity before alerting (closes the
  // stale-TID alert-leak window).
  //
  // Two uint32_t fields rather than `ThreadHandle by value` to
  // avoid pulling thread_lifecycle.h here (cycle via
  // futex_utils.h → wait_slot.h). owner_task_id == 0 ⇒ "unbound"
  // (slot was allocated before the lifecycle subsystem was up, or
  // by a foreign thread); validated alert helpers fall through to
  // unvalidated alerts in that narrow window.
  //
  // Identity protocol: `task_id` is monotonic 30-bit, never
  // recycled in-process — `registry_resolve(handle)` cannot
  // return the wrong thread on a positive match. `tid` is along
  // for the ride: it's the syscall arg used after resolve confirms
  // liveness.
  //
  // Layout: 8 bytes occupied by the two ids plus 4 bytes of
  // trailing padding so `filter_arg` lands at offset 44 (asserted
  // below).
  uint32_t owner_tid{0};
  uint32_t owner_task_id{0};
  uint32_t _pad_owner{0};

  // Waker-evaluated predicate / filter. Populated by
  // Futex::wait_on_predicate at Phase 2 setup; cleared by
  // clear_slot_owned. nullptr ⇒ no filter (unconditional wake).
  //
  // Role: pop_and_signal_one / signal_first_match_after call
  // filter_fn(value_.load(ACQUIRE), filter_arg) BEFORE pre-mark.
  // FALSE = skip this waiter (leave parked). Waker-side heuristic
  // only — does NOT govern the waiter's own exit condition.
  //
  // Read contract — waker-side (concurrent with slot reuse):
  //   1. Load slot.generation (ACQUIRE) -> gen_pre
  //   2. Load filter_fn (RELAXED)
  //   3. Load filter_arg (RELAXED)
  //   4. Re-load slot.generation (ACQUIRE) -> gen_post
  //   5. gen_pre != gen_post ⇒ pair may be torn; treat as "no
  //      filter this cycle" (unfiltered wake; the pre-mark CAS
  //      with stale snap will fail and force retry).
  //   6. Else: fn==nullptr ⇒ unfiltered; else call fn(load(), arg).
  //
  // Write contract — owner-side:
  //   - Phase 2 push setup: unconditional RELAXED stores of the
  //     wait's filter (HasPredicate=true: caller's pred/arg;
  //     HasPredicate=false: nullptr/0). Owner-exclusive — slot is
  //     off-chain, no other writer.
  //   - Between push and clear_slot_owned: immutable; wakers see
  //     stable values from the owner's push release.
  //   - In clear_slot_owned: NOT cleared. Wakers only consult
  //     filter_fn / filter_arg from chain-resident slots
  //     (state == WAITING || IN_KERNEL); IDLE-state filter is
  //     dormant. The next Phase 2 push setup's unconditional
  //     rewrite makes any clear here pure waste.
  //
  // RELAXED loads/stores everywhere; aligned 4/8-byte accesses
  // are machine-atomic. The gen-bracket is the torn-pair guard.
  // Order is load-bearing for layout: arg (u32) at 44, fn (u64)
  // at 48 — reversing forces 4 bytes of compiler-inserted padding.
  cpp::Atomic<uint32_t> filter_arg{0};
  cpp::Atomic<PredicateFn> filter_fn{nullptr};

  // Per-waiter wake mask for futex_addr (FUTEX_WAIT_BITSET). Slot
  // is eligible iff (wake_bitset & waker's M_k) != 0. Default ~0u
  // so classical FUTEX_WAIT/WAKE callers (no bitset) match
  // unchanged (waker's classical FUTEX_WAKE is treated as M_k =
  // ~0u).
  //
  // Owner writes pre-bucket-lock-held SLL insert (TLS-exclusive,
  // RELAXED safe). Wakers read under bucket lock — visibility via
  // bucket.lock release/acquire, no atomic annotation needed.
  // Unused by futex_utils::Futex (its wake filter is filter_fn).
  uint32_t wake_bitset{0xFFFFFFFFu};

  // Trailing pad bringing the struct to exactly 64 bytes.
  uint32_t _pad2{0};
};

// Layout asserts — order is load-bearing (see filter_arg comment).
static_assert(sizeof(WaitSlot) == 64,
              "WaitSlot must fit in one 64-byte cache line");
static_assert(alignof(WaitSlot) == 64,
              "WaitSlot must be cache-line aligned");
static_assert(__builtin_offsetof(WaitSlot, filter_arg) == 44,
              "filter_arg @ 44 ahead of filter_fn so the 8-byte fn "
              "pointer aligns naturally at 48 (no inserted padding)");
static_assert(__builtin_offsetof(WaitSlot, filter_fn) == 48,
              "filter_fn @ 48; reversing forces 4B compiler padding");
static_assert(__builtin_offsetof(WaitSlot, wake_bitset) == 56,
              "wake_bitset @ 56; trailing u32 pad fills the line");

// Validated alert helpers — close the stale-TID alert-leak window.
// Every waker site that would otherwise issue alert_one /
// alert_multiple routes through one of these:
//
//   1. Resolve the captured ThreadHandle via registry_resolve
//      under a Crystalline reservation. task_id is monotonic
//      30-bit and never recycles in-process, so a positive
//      resolve cannot return the wrong thread.
//   2. Live: issue the alert under the same Crystalline-pinned
//      lifecycle. "Alert in flight" classification on the
//      receiver side is driven structurally by
//      LINK_ALERT_FIRED_BIT on slot.link, published atomically
//      by the pre-mark CAS that preceded this call — no per-
//      thread RELEASE counter bump against the target's
//      lifecycle is needed.
//   3. Dead/recycled: skip the alert — do NOT pass the captured
//      TID to alert_one (closes the leak).
//
// "Unbound" handles (task_id == 0): slot allocated before the
// lifecycle subsystem came up, or by a foreign thread. Helpers
// fall through to unvalidated alerts on unbound handles
// (preserves pre-fix behavior in that narrow window).
//
// Owner reference is stored on the slot itself (owner_tid +
// owner_task_id); call sites pass a packed 64-bit handle to keep
// per-target storage tight in batched paths.
//
// Per-target record for batched wake paths (notify_all,
// drain_waiters, futex_addr::wake). Collection-pass walks write
// one of these per slot they pre-mark; alert_multiple_if_live
// reads them in order. The packed handle, captured tid, and pool
// index land on the same cache line, so the fast-path TID check
// and slow-path resolve touch one line per slot.
struct BatchTarget {
  uint64_t owner_packed; // ThreadHandle::pack() captured at pre-mark
  uint32_t tid;       // captured slot.thread_id at pre-mark time
  uint32_t slot_idx;  // wait_slot pool index — for get_slot(idx).thread_id
};

// Fast-path-capable overload. `slot` is a reference to the waker's
// target slot, live at call time. alert_one_if_live reads
// slot.thread_id as a cheap gate: if the slot's TID still matches
// the captured `tid`, the slot has not been freelist_push'd (which
// zeroes thread_id) and not reallocated, so the captured tid still
// names the same thread the waker intended. Fast path issues an
// unvalidated alert and skips registry resolve. Slow path (TID
// mismatch) validates via the registry.
void alert_one_if_live(uint64_t owner_packed, uint32_t tid, WaitSlot &slot);

// Slow-only overload for buffered-alert call sites where the slot
// may have been freed or reused by the time we alert (e.g.,
// parking-lot single-wake flush after bucket.lock release).
// Always validates via registry resolve.
void alert_one_if_live(uint64_t owner_packed, uint32_t tid);

// Build the captured ThreadHandle (as packed u64) from a WaitSlot's
// owner fields. Syntactic sugar for waker call sites.
LIBC_INLINE uint64_t owner_handle_packed(const WaitSlot &s) {
  return (static_cast<uint64_t>(s.owner_task_id) << 32) |
         static_cast<uint64_t>(s.owner_tid);
}

// Mark the current thread as expecting a late latched alert on its
// next alertable wait. Called from:
//   - drain_waker_alert when its in-cycle drain timed out without
//     consuming.
//   - SIGNALED-observation sites that exit without consuming the
//     alert (e.g., Phase 4 entry SIGNALED check), driven by
//     slot.link.is_alert_fired() — the bit's structural publish
//     replaces the prior position-based inference and the
//     futex_wake_epoch counter.
// No-op if the current thread has no lifecycle (foreign/pre-init).
//
// Sticky-flag semantics: if no alert ever follows (waker was
// killed after the pre-mark CAS but before the alert syscall), the
// flag remains set indefinitely. Benign — the `state == SIGNALED`
// check in Phase 4 fires before classify_exit, so the flag never
// eats a real wake; any eventual stray/stale STATUS_ALERTED
// harmlessly consumes it with a single reloop. The flag cannot
// escape the thread, cannot cascade, and cannot corrupt any
// shared state.
//
// Cross-subsystem alerts (cancel_support, registry_alert_all,
// stray ThreadLocalWord alerts that land while we're futex-parked)
// can also consume the flag. Harmless: the worst case is an
// additional reloop on a thread that wasn't going to need EINTR
// semantics anyway. LINK_ALERT_FIRED_BIT is the source of truth
// for "FUTEX alert in flight"; the flag is the cross-cycle
// persistence mechanism (alerts latch on the thread's NT alert
// flag, not on a slot — different cycles can use different slots).
void mark_expect_late_alert();

// Atomically read-and-clear the current thread's late-alert flag.
// Returns true iff a late alert was expected prior to this call.
// Called by Phase 4 on every STATUS_ALERTED + state-non-SIGNALED
// event. Idempotent: repeated calls without an intervening mark
// return false. No-op false when the current thread has no
// lifecycle.
bool consume_expect_late_alert();

// Phase 4 stale-latched-alert classifier. Distinguishes a real
// signal-style wake (fresh STATUS_USER_APC) from a stale latched
// alert (prior cycle's FUTEX waker alert that landed after that
// wait returned and surfaces on this cycle's first alertable
// wait). Caller MUST gate on slot.link state still being non-
// SIGNALED — a SIGNALED observation means the waker committed via
// slot.link and the alert is the publish, not a stale latch.
//
// The single discriminator is the per-thread expect_late_alert
// flag, set at SIGNALED-observation sites whose link snap carried
// LINK_ALERT_FIRED_BIT (waker pre-marked at IN_KERNEL → alert was
// fired) and at drain misses. Cross-thread RELEASE-bumps on a
// per-thread counter are no longer needed — the bit publishes
// "alert in flight" structurally on the wake's own atomic.
//
// Returns true ⇒ caller reloops (stale latch consumed).
// Returns false ⇒ caller treats as signal-style (Interruptible
// EINTR / non-Interruptible reloop after timeout refresh).
[[nodiscard]] LIBC_INLINE bool classify_stale_alert(NTSTATUS status) {
  return status == STATUS_ALERTED && consume_expect_late_alert();
}

// Batched validated alert. Per entry: if
// get_slot(slot_idx).thread_id == captured tid, the slot hasn't
// been recycled — bypass registry resolve (fast path).
// Slow-path (TID mismatch) defers to validated resolve under a
// pin taken lazily on first slow arrival, so all-fast-path
// broadcasts pay no pin/unpin. Single alert_multiple syscall for
// all survivors.
uint32_t alert_multiple_if_live(const BatchTarget *tgts, uint32_t count,
                                 HANDLE *out_filtered, void *ab_ctx,
                                 uint32_t ab_ctx_count);

// Compact target for stack-steal wake paths whose walk's
// IN_KERNEL→SIGNALED_CLEAN CAS-with-snap IS the state-transition
// authority. Post-CAS TID mismatch ⇒ owner already woke via
// another alert and returned the slot — skip without registry
// resolve. 8B vs BatchTarget's 24B → 3× scratch density, no
// owner_ref capture needed. NOT for futex_addr::wake (its
// pre-mark CAS doesn't own a state-transition exchange and still
// needs validated slow path).
struct CompactTarget {
  uint32_t tid;
  uint32_t slot_idx;
};

// Stack-steal alert. Per entry: TID-match push, mismatch skip. No
// pin, no resolve, no bump. Single alert_multiple syscall.
uint32_t alert_multiple_if_live_compact(const CompactTarget *tgts,
                                         uint32_t count, HANDLE *out_filtered,
                                         void *ab_ctx, uint32_t ab_ctx_count);

// Slot 0 = null sentinel.
inline constexpr uint32_t NULL_INDEX = 0;

// Pool and freelist live in an anonymous namespace in the .cpp;
// these functions are the public interface.

// Calling thread's slot index (TLS fast path). Allocates on first use.
uint32_t get_slot_index();

WaitSlot &get_slot(uint32_t index);

// Secondary slot for nested waits (VEH during futex wait). Returns
// NULL_INDEX if pool exhausted; caller MUST release_secondary().
uint32_t alloc_secondary();
void release_secondary(uint32_t index);

// Refresh TLS gen for `index` to `new_gen` (caller's bumped value).
// Called by clear_slot_owned so the owner's next fast-path reuse
// compare matches. Caller-supplied new_gen avoids a second ACQUIRE
// in the steady completion path. No-op when TLS tracks a different
// slot (the cleared slot is a secondary; TLS tracks primary only).
void refresh_tls_slot_generation(uint32_t index, uint32_t new_gen);

// Return a slot to the freelist (TLS cleanup on thread exit).
void free_slot(uint32_t index);

// Typed proof of detach for reclaim_slot. Constructible only via
// the four named factories — each documents a distinct detach
// origin. reclaim_slot rejects any path that cannot produce one,
// so a call site without proof of detach can't reach the entry
// point. Auditing reclaim sites = local review of the matching
// after_* factory call.
//
//   after_walker_splice      harris_unlink's parent CAS committed
//                            (includes inline-unlink and dead-
//                            intermediate splice).
//   after_stack_pop          single-entry stack_ CAS detach
//                            (pop_and_signal_one, handoff_one,
//                            drain_stale_top TIMED_OUT branch).
//   after_stack_steal        whole-stack steal CAS (notify_all,
//                            notify_all_chunked, drain_waiters).
//   after_unlinker_dispatch  thread-exit / Phase-1.75 self-recovery
//                            via a registered subsystem unlinker
//                            that returned true, or no chain
//                            residency to begin with (orphan).
class ReclaimAuthority {
public:
  LIBC_INLINE constexpr uint32_t index() const { return index_; }

  LIBC_INLINE static constexpr ReclaimAuthority
  after_walker_splice(uint32_t idx) {
    return ReclaimAuthority{idx};
  }
  LIBC_INLINE static constexpr ReclaimAuthority
  after_stack_pop(uint32_t idx) {
    return ReclaimAuthority{idx};
  }
  LIBC_INLINE static constexpr ReclaimAuthority
  after_stack_steal(uint32_t idx) {
    return ReclaimAuthority{idx};
  }
  LIBC_INLINE static constexpr ReclaimAuthority
  after_unlinker_dispatch(uint32_t idx) {
    return ReclaimAuthority{idx};
  }

private:
  LIBC_INLINE constexpr explicit ReclaimAuthority(uint32_t idx)
      : index_(idx) {}
  uint32_t index_;
};

// Transition slot to IDLE+CERT, bump generation, and push to the
// freelist. The ReclaimAuthority parameter is the typed proof of
// detach (audited at construction). Direct push is safe under the
// state+tag fold — see wait_slot.cpp:reclaim_slot for the rationale.
void reclaim_slot(ReclaimAuthority auth);

// --- Stale-slot recovery (owner-side self-reclaim) ---

// If the TLS slot is TIMED_OUT (stale from a prior timed-out wait),
// return its index + load its wait_address + TLS-saved gen. Caller
// passes gen to the appropriate unlinker so it refuses against a
// reclaimed+reallocated slot. NULL_INDEX if no stale slot.
uint32_t get_stale_slot(uintptr_t &wait_address_out,
                        uint32_t &expected_gen_out);

// Clear the TLS ref after a stale slot has been unlinked and freed.
// Called by Futex::wait entry after self-reclaim.
void clear_tls_slot();

// --- Subsystem unlinker registration ---
//
// Thread-exit TLS cleanup needs to splice a dying thread's slot
// out of whatever it's parked on. wait_slot.cpp can't include
// futex_utils.h / futex_addr.h (both depend on this header), so
// each subsystem registers its own unlinker. Dispatch is by
// slot.subsystem.
//
// The callback refuses if the current slot gen != expected_gen
// (slot reclaimed + possibly reallocated to a different thread —
// splicing would corrupt an unrelated live waiter).
//
// Returns true ⇒ caller should reclaim_slot. False ⇒ another
// actor handled it (gen mismatch), or the waker popped it
// mid-dispatch (caller may still need to free, but NOT via
// reclaim_slot).
using HarrisUnlinker = bool (*)(void * /* Futex* */, uint16_t /* idx */,
                                uint32_t /* expected_gen */);
using ParkingLotUnlinker = bool (*)(void * /* user addr */,
                                    uint16_t /* idx */,
                                    uint32_t /* expected_gen */);
void register_harris_unlinker(HarrisUnlinker fn);
void register_parking_lot_unlinker(ParkingLotUnlinker fn);

// Installs both subsystem unlinkers. Defined in
// futex_subsystem_register.cpp (which includes the header-only
// futex_{addr,utils}.h to take function addresses). Called once.
void install_subsystem_unlinkers();

void init();        // Startup: TLS allocation, freelist setup.
void fini();        // Shutdown: TLS free.
void fork_reinit(); // Reset the pool in the fork child.

} // namespace wait_slot
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_WAIT_SLOT_H
