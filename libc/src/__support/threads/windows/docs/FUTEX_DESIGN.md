# Windows Futex Design

Long-form protocol notes for `futex_utils.h`, `wait_slot.h`, `wait_slot.cpp`,
and `futex_addr.h`. Headers cite sections of this document by name (e.g.
"see FUTEX_DESIGN.md §LINK_CERT_BIT") in place of inline narrative.

This is a derivation reference, not an API guide — function signatures and
caller contracts live next to the code. Sections are organised by protocol
concern, not by file.

---

## §Combined word

```
combined_  : cpp::Atomic<uint64_t>     // [value_:32 (upper LE) | stack_:32 (lower)]
  value_   : cpp::Atomic<FutexWordType> // user-visible futex word
  stack_   : cpp::Atomic<uint32_t>      // [gen:16 | top:16] Treiber wait stack
```

A single CAS-64 on `combined_` atomically verifies the user value AND pushes
the waiter — no Dekker re-check, no lost-wakeup window, no cancel ghosts.
Native RMW on `value_` (fetch_add, etc.) does not touch `stack_`; CAS-32 on
`stack_` (pop) does not touch `value_`.

`stack_` packs `[gen:16 | top:16]` where `top` indexes into the WaitSlot
pool (`STACK_NULL` = pool index 0 = sentinel). `gen` is the 16-bit ABA
counter on the head pointer; per-slot `link.tag:32` is the wider per-link
ABA defence.

LIFO wake order: most recently parked thread wakes first (cache-warm,
matches Linux qspinlock / parking_lot).

---

## §SlotState

`slot.link` low-level layout: `[state:8 | reserved:8 | tag:32 | next:16]`.
The state byte fold collapses three independent pieces of state into one
atomic word:

1. Stack-linkage lifecycle (IDLE → WAITING → IN_KERNEL → SIGNALED_*).
2. Wake signal (SIGNALED_* family).
3. Wake kind + cleanup responsibility (CLEAN/ORPHAN, plain/HANDOFF).

```
IDLE → WAITING ┬→ IN_KERNEL ┬→ SIGNALED_CLEAN             → IDLE
               │            ├→ SIGNALED_ORPHAN            → IDLE
               │            └→ TIMED_OUT                  → IDLE
               ├→ SIGNALED_CLEAN                          → IDLE
               ├→ SIGNALED_ORPHAN                         → IDLE
               ├→ SIGNALED_HANDOFF_CLEAN                  → IDLE
               ├→ SIGNALED_HANDOFF_ORPHAN                 → IDLE
               └→ TIMED_OUT (owner cancel)                → IDLE
```

State semantics:

| State                       | Meaning                                                  |
|-----------------------------|----------------------------------------------------------|
| `IDLE`                      | slot off any chain, owner-exclusive                      |
| `WAITING`                   | pushed onto Treiber stack, not yet in kernel             |
| `IN_KERNEL`                 | blocked in NtWaitForAlertByThreadId                      |
| `SIGNALED_CLEAN`            | waker pre-marked + detach CAS won; slot off-stack        |
| `SIGNALED_ORPHAN`           | waker pre-marked but detach CAS lost; slot still linked  |
| `SIGNALED_HANDOFF_CLEAN`    | CLEAN + ownership transferred (HANDOFF_BIT)              |
| `SIGNALED_HANDOFF_ORPHAN`   | ORPHAN + ownership transferred                           |
| `TIMED_OUT`                 | owner self-cancel; logical-deletion marker on chain      |

`HANDOFF` is emitted only by `handoff_one`'s WAITING branch — never to an
IN_KERNEL waiter (alert-loss risk).

`TIMED_OUT` is the logical-deletion marker — no separate next-pointer mark
bit needed because Treiber stacks only insert at the head, so Harris's
classical mid-chain insert protection is unnecessary. Walkers read the
state byte as part of the link word to identify dead nodes for
opportunistic splice.

Encoding is chosen so that `state != WAITING` exits the byte-7 fast-path
spin (`spin_on_link_state`) on any wake.

`is_valid_state_transition` (in `wait_slot.h`) is the runtime-callable,
debug-asserted version of the diagram above; the state-CAS helpers
(`link_cas_state`, `link_cas_state_certify`, `link_cas_state_detached`)
template on `<From, To>` and `static_assert` the validity table at compile
time. Self-transitions (X → X) are not legal — every CAS bumps tag, and a
same-state CAS implies the caller misread the protocol.

The re-arm path `SIGNALED_*_CLEAN → WAITING` (used by `inkernel_repark`)
goes through `link_store_state`, a single-writer unconditional store that
bypasses the validity table by design — the re-arm is well-understood and
exclusive to the slot's owner.

---

## §LINK_MARK_BIT

Reserved bit 48 of `slot.link`. Load-bearing for lock-free mid-chain
splice in the Harris walker.

The walker has a two-word problem: it CASes `parent.link` using a `next`
captured from `slot.link`, and those are different atomic words. Without
a mark, a concurrent op-splice of slot's successor (which CASes
`slot.link` to advance `slot.next`) between our `slot.link` read and our
`parent.link` CAS leaves us CASing `parent.next` to a stale (now off-chain
or reclaimed) slot — chain-disconnect, double-reclaim of a TIMED_OUT
successor, or IDLE-on-reachable-chain.

### Mark-then-help protocol (robust to splicer death)

1. CAS-set `LINK_MARK_BIT` on `slot.link` (expected unmarked, tag bumped,
   state/next preserved).
2. Read `slot.next` from the marked link — guaranteed stable: any other
   CAS on `slot.link` fails on tag or mark mismatch.
3. CAS `parent.link` with `slot.next` as `desired.next`.
4. **Success**: slot is off-chain. Single strong CAS via `with_cert_unmark`
   atomically clears MARK and sets CERT (see §LINK_CERT_BIT).
5. **Failure**: walker `link_clear_mark()` to release, then restart.

### Help-not-wait on observed MARK

A walker that observes `MARK=1` on another slot's `slot.link`
completes the splice on the marker's behalf rather than spinning. The
observed marked link IS structurally the same value the marker's
`link_cas_set_mark` wrote — wrapped as a `MarkedLinkSnap` via
`MarkedLinkSnap::from_observed_marked` (typed factory; debug-asserts
`is_marked()` on the input), the helper re-runs steps 3–5:

- **Helper's parent CAS succeeds**: slot is off-chain by our CAS. Helper
  finalizes via `link_finalize_after_splice`. Strong CAS at finalize
  means at most one finalize wins — any racing finalize harmlessly
  fails.
- **Helper's parent CAS fails**: another actor won the parent CAS, or
  the chain shape changed and the splice is no longer applicable
  (e.g. parent transitioned dead between mark and help). Helper does
  NOT call finalize on this branch — finalizing without proof of
  off-chain would publish CERT on a slot that may still be on-chain
  via the dead parent. Helper calls `link_clear_mark` to release the
  freeze; the next walk iteration re-evaluates from scratch.
  `link_clear_mark` is idempotent on `MARK=0` (covers the
  marker-already-finalized branch).

Strong CAS at both the parent rewrite and the finalize is the
serialization point: at most one winner per CAS, losers bail
harmlessly. The protocol is robust to mid-splice marker death — no
walker observing MARK=1 waits on the marker's continued execution.

### Mark preservation by waker/owner primitives

Waker/owner CAS/XCHG primitives must preserve the mark bit. `link_cas_snap`,
`link_cas_state`, `link_exchange_state`, `link_cas_state_detached` all OR
the expected's mark bit into desired (via `LINK_PRESERVE_MASK`), so a
concurrent walker splice in progress (its own or a helper's) is not
silently unblocked mid-protocol.

---

## §LINK_CERT_BIT

Reserved bit 49 of `slot.link`. Structural fold of the procedural invariant

> "`clear_slot_owned` writes IDLE only on a slot proven off-chain"

into a single atomic bit. The load-bearing direction is `CERT==1 ⇒
off-chain`. CERT==0 has the *weaker* "not certified" semantics — never
code against `CERT==0 ⇒ on-chain`.

CERT=1 is set by every operation that proves the slot is off any chain at
the moment the bit is set:

| Setter                              | Site                                            |
|-------------------------------------|-------------------------------------------------|
| `link_cas_state_certify`            | waker upgrade ORPHAN → CLEAN post-detach; stack-steal CAS-fail-on-orphan follow-up |
| `link_finalize_after_splice`        | walker splice success (atomic with MARK clear) |
| `link_cas_state_detached`           | stack-steal CAS-with-snap (CAS-success path)   |
| `link_exchange_state_to_idle_certify` | reclaim's IDLE write                          |
| `Link::pack_certified`              | freelist push, fresh-page init                  |
| `link_field_set_cert`               | Phase 2 / repark bail re-publish                |

CERT=1 is *cleared* by exactly one operation: `link_store_next_uncertify`
(Phase 2 / `inkernel_repark` push loop). The slot is about to be committed
onto a chain via the subsequent `combined_` CAS; revoke any prior off-chain
certificate so on-chain residency reads CERT=0 (truthful).

All other writers passively PRESERVE CERT via `LINK_PRESERVE_MASK`, so
the bit's semantics never silently flip during state transitions.

Read by:

- `clear_slot_owned` — fast path on `CERT=1 && MARK=0`; CAS-loop slow path.
- `get_slot_index` TLS fast-path — requires CERT=1 to reuse a slot.
- `slot_cleanup` — dispatches reclaim vs unlink based on CERT.

**Internal-only encoding window**: between `link_store_next_uncertify`
(CERT=0) and the subsequent `combined_` CAS commit, slot is off-chain
with CERT=0 (transient lie). This window is owner-exclusive and owner is
mid-Phase-2-push; no consumer reads CERT in this window. On Phase 2 /
repark bail, an explicit `fetch_or(LINK_CERT_BIT)` at the bail site
re-certifies before `clear_slot_owned` consumes it.

**State implication**: `state == SIGNALED_CLEAN` (or `_HANDOFF_CLEAN`)
implies CERT=1 by construction (the only paths that produce CLEAN are
the certify-fold paths). Wakers promote to CLEAN only after a successful
detach CAS.

---

## §Link wither families

`Link` wraps the 64-bit packed word with typed accessors and tag-bumping
withers. Tag monotonicity is a structural type-level invariant: every
wither bumps tag exactly once, and the `uint64_t`-taking constructor is
private. Constructing a Link without a static factory (no prior history)
or a wither (tag bumped) is impossible.

Single-bump compound withers (`with_state_set_cert`, `with_cert_unmark`,
`with_state_certified`) exist so chained `with_state(s).certified()` /
`unmarked().certified()` patterns at detach-prover and splice-finalize
sites collapse to a single tag bump. Two-bump chains are correct
(strictly-greater is sufficient for ABA defence) but wasteful — each
unnecessary bump halves the per-slot 32-bit wraparound distance.

| Family | Withers | Purpose |
|--------|---------|---------|
| **Pure** | `with_state`, `with_next`, `marked`, `unmarked`, `certified`, `uncertified` | Preserve every other field including MARK and CERT. |
| **Owner-exclusive (drop MARK)** | `rewrite_certified`, `with_state_drop_mark`, `with_next_drop_mark`, `with_next_uncertify`, `with_state_certified` | Used at sites where caller is owner-exclusive on a slot proven off-chain; carrying a stale MARK forward could freeze a future walker. |
| **Single-bump compound** | `with_state_set_cert`, `with_cert_unmark` | Combine state+CERT or MARK-clear+CERT into one atomic transformation. |
| **Static factories** | `pack`, `pack_marked`, `pack_certified`, `from_raw` | Fresh construction with no "old" value to derive from. Used at slot-pool init, freelist init, atomic-load boundaries. |

`from_raw` and `.raw()` exist for the few cases where round-trip
serialisation to a 64-bit integer is needed (debug instrumentation,
`link_field_set_cert` which uses `__atomic_fetch_or` on the underlying
word). Not needed for normal load/store/CAS.

---

## §SubsystemKind

Identifies which wait primitive a slot is currently linked to. Required
by thread-exit cleanup so it can dispatch to the correct unlink path —
calling `Futex::harris_unlink` with a user address from the parking-lot
path would walk arbitrary memory.

`enum class SubsystemKind : uint8_t { None, Futex, ParkingLot }`.
Storage type is `uint8_t` so `cpp::Atomic<SubsystemKind>` carries no
extra width over a plain atomic byte. The typed API forbids implicit
conversion to/from integral types: `subsystem + 1` or `== 0` is a
compile error.

`Futex::try_from_owner_slot` and `futex_addr::try_user_addr_from_owner_slot`
are typed accessors gated on `subsystem == Futex` / `ParkingLot`
respectively; they return `nullptr` on mismatch. The Phase 1.75 stale-
slot recovery path dispatches through these accessors — the cast
from `wait_address` to `Futex*` / user-address lives behind the
accessor where the type tag governs it structurally.

---

## §Wait phases

```
Phase 1     hardware spin on value_ (UMWAIT/MWAITX)
Phase 1.5   timeout already expired?
Phase 1.75  owner-side stale-slot reclaim (prior TIMED_OUT TLS)
Phase 2     CAS-64 push onto Treiber stack (atomic value+push)
Phase 2.5   pre-kernel cache spin on slot.link state byte
Phase 3     CAS state WAITING → IN_KERNEL
Phase 4     NtWait sleep + wake/timeout/stale-alert/signal loop
```

Early-exit returns:

| Phase | Trigger                                  | Return                              |
|-------|------------------------------------------|-------------------------------------|
| 1     | value changed                            | 0                                   |
| 1.5   | timeout expired                          | -ETIMEDOUT                          |
| 2     | pool exhausted                           | -ENOMEM                             |
| 2     | CAS-64 cond.satisfied(val)               | 0                                   |
| 2.5   | SIGNALED state observed                  | HANDOFF-decoded                     |
| 3     | CAS-fail self-commit ORPHAN              | HANDOFF-decoded                     |
| 4     | SIGNALED state observed                  | HANDOFF-decoded                     |
| 4     | STATUS_TIMEOUT                           | cancel_or_absorb(-ETIMEDOUT)        |
| 4     | stale alert + predicate moved off        | cancel_or_absorb(0)                 |
| 4     | stale alert + timeout expired            | cancel_or_absorb(-ETIMEDOUT)        |
| 4     | signal-style [Interruptible]             | cancel_or_absorb(-EINTR)            |
| 4     | signal-style [!Interruptible] + timeout  | cancel_or_absorb(-ETIMEDOUT)        |
| 4     | signal-style [!Interruptible]            | reloop                              |

All `cancel_or_absorb` paths decode HANDOFF on waker race, so a late
wake that lost the cancel CAS still delivers its handoff.

`Interruptible=true` makes APC/signal wakes return -EINTR instead of being
absorbed as spurious. Zero codegen impact on the default path — the
`if constexpr` branches in `wait_impl` are eliminated.

`HasPredicate=true` selects the caller-supplied `PredicateFn` stop
condition (`cond.pred(v, arg)`) over the classic `v != arg`. Compile-time
pruning via `if constexpr` keeps the classic path byte-equivalent to the
pre-template `wait_impl`.

`wait_impl` is a thin wrapper around `wait_one_cycle` providing the
predicate-aware top-level absorb loop: if `wait_one_cycle` returned 0 but
pred is still FALSE (rare — possible when the in-kernel re-park exits
via a non-pred path like invalidation race), re-enter the cycle. The loop
is compiled out for `HasPredicate=false`.

### Phase 4 in-kernel re-park (CLEAN-only)

Phase 4 thundering-herd absorption: waker published `SIGNALED_*_CLEAN`
but pred is still FALSE (winner flipped value_; losers don't satisfy).
Re-install the already-owned slot in place rather than paying Phase 0-3
re-entry:

1. `link.state → WAITING` (tag bump, next preserved).
2. Re-publish `subsystem = Futex` (waker cleared it). `wait_address` /
   `filter_fn` survive from the original Phase 2 install.
3. CAS-64 push back onto the stack; bail Satisfied on pred flip mid-retry.
4. Phase 2.5 cache spin + Phase 3 CAS → IN_KERNEL, mirroring the original
   wake-decode branches.

CLEAN only — CLEAN means the waker's detach CAS committed, so the slot is
guaranteed off-chain. ORPHAN waves bail to top-level absorb in
`handle_pred_signaled_wake` (re-pushing a still-linked slot would cycle
the chain). Tag monotonicity across (1)-(5) kills any stale-snap publish
from a prior wake wave.

---

## §Waker protocol (B'/A'/U/K)

Step labels used by `pop_and_signal_one`, `handoff_one`,
`signal_first_match_after`:

| Label | Action                                                                    |
|-------|---------------------------------------------------------------------------|
| `[B']` | Pre-mark via `link_cas_snap` → `SIGNALED_*_ORPHAN`.                       |
| `[A']` | Detach CAS on `stack_` (best-effort; failure ⇒ slot stays ORPHAN).        |
| `[U]`  | Best-effort upgrade ORPHAN → CLEAN on detach success (sets CERT atomic).  |
| `[K]`  | Alert IN_KERNEL waiters (unconditional — `[B']` committed the wake).      |

Cost: 2 LOCK'd RMWs + ~1 for `[U]`. Constant regardless of stack depth;
ORPHAN waiters pay O(D) self-splice on their own wake path.

The pre-mark in step `[B']` is a single atomic that publishes
EVERYTHING — wake signal + wake kind + cleanup responsibility + tag bump.
This closes the Harris walker's mid-splice race (any walker that captured
this slot as `prev` finds its mid-splice CAS naturally failing on the
pre-mark) AND the old wake_word race window (no separate publish can
drift cross-cycle).

### `[U]` upgrade detail

After a successful detach CAS, the waker attempts
`link_cas_state_certify<ORPHAN, CLEAN>` as a best-effort upgrade. The
race with the waiter's post-wake decode is benign — at worst the waiter
does one no-op walk via `self_splice_if_orphan`. The CAS ALSO publishes
CERT atomically with the state transition, satisfying
`clear_slot_owned`'s precondition.

### `self_splice_if_orphan` — structural off-chain proof

The owner's wake epilogue calls `self_splice_if_orphan` whenever it
observes a `SIGNALED_*_ORPHAN` state. The function must return with
slot.link in a state where `clear_slot_owned`'s subsequent IDLE+CERT
write is correct — i.e., the slot is provably off-chain.

The function:

1. Load slot.link. If state is non-orphan or CERT=1, return.
2. Call `harris_unlink` once. With mark-then-help on observed MARK
   (see §LINK_MARK_BIT), `harris_unlink` always terminates and on
   return is in one of three states:
   - *Spliced by us* — our `link_finalize_after_splice` published
     CERT atomically (returns true).
   - *Spliced by another walker / waker* — their commit path
     published CERT (returns false: NotFound or SplicedNoReclaim).
   - *Walked-to-NULL with no splice* — already off-chain when we
     entered, no publisher was committed because none was needed.
3. Unconditional `link_field_set_cert` (`fetch_or` of
   `LINK_CERT_BIT`). Idempotent for cases (a)/(b); structurally
   closes case (c).

**Owner authority for the unconditional CERT publish.** In
`SIGNALED_*_ORPHAN`, only the owner can transition the slot back to
a chain-resident state — Phase 2 push requires the owner-exclusive
IDLE → WAITING transition through `clear_slot_owned`'s terminal
write. Therefore once off-chain, the slot stays off-chain until owner
itself reuses it. Owner is the canonical off-chain witness: a
`harris_unlink` that returned without finding self IS structural
proof of off-chain, and owner publishes CERT on that proof.

**Termination is structural.** `harris_unlink` terminates because
every walk iteration either advances, splices, helps complete a
foreign splice, or returns. The CERT publish is one `fetch_or` —
single atomic, no loop. No path through this function depends on
another thread's continued execution.

### Stack-steal CERT closure (CAS-fail-on-orphan)

`notify_all` / `notify_all_chunked` / `drain_waiters` steal the entire
stack via one CAS on `stack_`, then walk the stolen chain calling
`link_cas_state_detached<SIGNALED_CLEAN>` per slot. The per-slot CAS
expects the snap loaded during pipeline fill. If a racing waker
pre-marked the slot to `SIGNALED_*_ORPHAN` between snap-load and the
per-slot CAS, the CAS fails on state mismatch — the slot is off-chain
(by the earlier stack_ CAS) but the failing CAS never published CERT.

Without follow-up, the slot would be off-chain in
`SIGNALED_*_ORPHAN+CERT=0` indefinitely. The owner's
`self_splice_if_orphan` would loop on UMWAIT for CERT that never
lands (no publisher), and `slot_cleanup` at thread exit would
similarly fail to reclaim (CERT=0 → dispatch unlinker → walks empty
chain → returns false → re-checks CERT=0 → declines reclaim → leak).

Closure: every CAS-fail-on-`SIGNALED_*_ORPHAN` follows up with
`link_cas_state_certify<ORPHAN, CLEAN>` (or the HANDOFF variant) to
publish CERT. The slot is provably off-chain (by the steal); the
certify completes the protocol. After this, "stack-steal removes ⇒
CERT published" is total across all CAS outcomes.

This closes a latent leak in the `slot_cleanup` thread-exit path on
stack-stolen orphans, and makes `self_splice_if_orphan`'s wait
guaranteed to converge.

### Store-before-alert (load-bearing)

Every Completed path in `handoff_one`'s IN_KERNEL branch does
`value_.store(unlock_val, SEQ_CST)` BEFORE the wake (cache publish and/or
alert syscall). Inverting is the **16T starve vector**:

> waker preempted between alert and store → waiter wakes → re-CASes value_
> which is still LOCKED → re-parks → repeat = livelock.

Observed in `futex_bench`. SEQ_CST is required because RawMutex's
`exchange(IN_CONTENTION, ACQUIRE)` retry depends on globally-ordered
observation.

### Capture-tid-with-owner-ref (I4)

Wakers MUST capture `slot.thread_id` AND `owner_ref_of(slot)` at the
SAME call site, post-pre-mark and pre-detach. The pre-mark prevents the
owner from running `clear_slot_owned` and recycling the slot under us.
Without that ordering, the late `owner_ref` read can bind a recycled
slot's new owner while the captured tid names the old owner — wrong-TID
alert.

The `WakeCommitToken` type bundles the two reads behind a single factory
(`capture_wake_target`) so there is no "second sample" path for callers
to accidentally open.

### `handoff_one` WAITING branch — store transit before pre-mark

The pre-mark IS the single wake publish. The WAITING waiter is spinning
on `slot.link` in Phase 2.5 and can wake the moment the state byte
changes. So `value_.store(transit_val)` MUST happen BEFORE
`link_cas_snap` — any post-pre-mark store would race the waiter's claim
CAS (waiter `CAS(transit_val → IN_CONTENTION)` sees stale LOCKED, fails,
re-parks → handoff stranded).

**DO NOT roll value_ back to unlock_val on pre-mark fail.** A transient
UNLOCKED window would let `try_lock` steal the lock, then our IN_KERNEL
retry's later `store(UNLOCKED)` would clobber their LOCKED → double
ownership. Leaving value_ at transit_val is safe: try_lock sees TRANSIT
and fails; lock_slow parks on `wait(TRANSIT)`. The retry path (IN_KERNEL
or Empty) publishes UNLOCKED atomically.

---

## §Stale-latched-alert distinguisher

Distinguishes a real signal-style wake (fresh `STATUS_USER_APC` or
genuine cross-subsystem alert) from a stale latched alert — a prior
cycle's FUTEX waker alert that arrived after that wait returned,
latched on the thread, and surfaces on this cycle's first alertable
wait. Left unfiltered, stale latches spuriously upgrade to EINTR
(Interruptible) or to a spurious wake.

### Single-bit-driven distinguisher

The "alert in flight" signal lives **structurally on `slot.link`**:
`LINK_ALERT_FIRED_BIT` (reserved bit 50). The pre-mark CAS that commits
a wake sets the bit iff the snap state was IN_KERNEL — i.e., iff the
waker fires an unconditional alert. WAITING-state pre-marks are caught
by Phase 2.5 cache spin without a syscall and never set the bit.

Setters are single-atomic with the wake commit:

- `link_cas_state<IN_KERNEL, SIGNALED_*>` — `if constexpr` selects
  `with_state_alerting` so the bit publishes alongside the state byte.
- `link_cas_snap<SIGNALED_*>` — runtime selects `with_state_alerting`
  when `expected_snap.state() == IN_KERNEL`.
- `link_cas_state_detached<SIGNALED_*>` — same runtime selection on
  the pre-detach snap, used by `notify_all` / `drain_waiters`
  stack-steal walkers.

Clearers run at slot wake-cycle resets:

- `rewrite_certified` — freelist push, fresh page init, fork_reinit,
  post-pop alloc init.
- `with_state_certified` — `clear_slot_owned` terminal IDLE+CERT
  publish (owner consumed the wake).
- `with_next_uncertify` — Phase 2 / `inkernel_repark` push commit
  (slot enters a fresh wait cycle).

All other primitives preserve the bit through `LINK_PRESERVE_MASK`.

### Per-thread persistence: `expect_late_alert` flag

Latched alerts live on the thread's NT alert flag, not on a slot — the
next NtWait that consumes the latch may be on a *different* slot
(different futex, different cycle, VEH-nested secondary). The bit
publishes "alert was fired against this slot's owner", but cross-cycle
persistence requires per-thread state.

`expect_late_alert` (one byte in `ThreadLifecycle`) is set:

- By `drain_waker_alert` when its bounded in-cycle drain timed out
  without consuming.
- By every SIGNALED-observation site that exits without consuming the
  alert, gated on `snap.is_alert_fired()`. Phase 4 entry SIGNALED is
  the principal path — Phase 3 succeeded so state was IN_KERNEL when
  the waker captured snap, the bit is 1, the flag is set.

Consumed exactly once per latched alert by `classify_stale_alert(status)`
on `STATUS_ALERTED`. Idempotent: repeated calls without an intervening
mark return false.

The bit is the structural source of truth for "alert in flight"; the
flag is the cross-cycle persistence vehicle. Together they incur zero
cross-thread atomic writes — the bit rides for free in the existing
pre-mark CAS, and the flag is own-thread RELAXED.

### Sticky-flag escape hatch

If no alert ever follows (waker was killed after pre-mark CAS commit
but before the alert syscall), the flag remains set indefinitely.
Benign: the pre-wait state check in Phase 4 prevents the flag from
ever eating a real wake (real FUTEX wakes are observed via `slot.link`
state, not via `STATUS_ALERTED`), and any eventual stray/stale
`STATUS_ALERTED` harmlessly consumes it with a single reloop. The flag
cannot escape the thread, cannot cascade, and cannot corrupt any
shared state.

Cross-subsystem alerts (cancel_support, registry_alert_all, stray
ThreadLocalWord alerts that land while we're Futex-parked) can also
consume the flag. Harmless: with `LINK_ALERT_FIRED_BIT` on the slot,
any *future* FUTEX wake is still distinguishable via SIGNALED-state
observation (which fires before `classify_stale_alert`); the flag's
job is only to suppress *this cycle's* latched FUTEX alert, and a
cross-subsystem consumer of the flag at worst causes one extra reloop.

---

## §Validated alert helpers

Closes the stale-TID alert-leak window. Every waker site that would
otherwise issue `alert_one` / `alert_multiple` routes through these
helpers:

1. Pin the thread registry (EpochGuard).
2. Resolve the captured `(page, index, generation) SlotRef` via
   `registry_resolve_slot_ref` — returns the current `ThreadLifecycle*`
   for the slot iff its generation still matches (same thread, still
   registered). `nullptr` if recycled or deregistered.
3. On live resolve: issue the alert under the pinned lifecycle.
   "Alert in flight" classification on the receiver side is driven
   structurally by `LINK_ALERT_FIRED_BIT` on `slot.link`, set
   atomically by the pre-mark CAS that preceded this call — no
   cross-thread RELEASE bump on the target's lifecycle is needed.
4. On dead/recycled resolve: skip the alert. The captured TID is NOT
   blindly passed to alert_one — closes the leak.

`SlotRef` (12 bytes inline as page/index/generation in WaitSlot) is
the safe cross-thread reference type. A raw `ThreadLifecycle*` is
UAF-unsafe: lifecycle memory lives in a SlabPool whose pages can
transition PAGE_NOACCESS after retirement. `registry_resolve_slot_ref`
indexes the page array under pin; the page array invariant guarantees
entries never point at freed memory (retire CAS's the entry to
`nullptr` before pushing onto retired_pages).

"Unbound" refs (all fields `UINT32_MAX` or `0`) are a fallback for slots
allocated before the lifecycle subsystem was up, or by foreign threads
without registry presence. The helpers fall through to unvalidated
alerts on unbound refs.

### Fast-path overload (`alert_one_if_live(ref, tid, slot)`)

Reads `slot.thread_id` as a cheap gate: if it still matches the captured
`tid`, the slot has not been freelist-pushed (which zeroes thread_id)
and not reallocated, so the captured tid names the same thread. Fast
path issues an unvalidated alert and skips EpochGuard + resolve + epoch
bump. Slow path (TID mismatch) validates via the registry.

### Batched variants (`BatchTarget` / `CompactTarget`)

`BatchTarget` (24B: ref + tid + slot_idx) for batched wake paths whose
pre-mark CAS doesn't own a state-transition XCHG (notify_all of
parking-lot waiters; futex_addr::wake) — still requires the validated
slow path on TID mismatch.

`CompactTarget` (8B: tid + slot_idx) for stack-steal paths
(notify_all, drain_waiters) whose `IN_KERNEL → SIGNALED_CLEAN` XCHG IS
the state-transition authority. After a successful XCHG, thread_id
mismatch at alert time implies the owner already woke via an unrelated
alert — they don't need our wake, and no registry resolve is needed to
skip them. 3× scratch density vs BatchTarget; more entries fit in L1
for the alert-time re-check pass.

---

## §Walker race protocol

Actors: `W` (walker in `harris_unlink`), `P` (`pop_and_signal_one` /
`handoff_one`), `A` (`notify_all` / `drain_waiters` /
`notify_all_chunked`), `T` (Phase-2 push), `R` (`reclaim_slot` direct
freelist push).

### Protocol summary

- `T` head-only CAS-64; never rewrites mid-stack next.
- Timeout/EINTR/thread-exit: CAS state → TIMED_OUT (logical-deletion
  marker for walkers).
- `P` link_cas_snap → SIGNALED_* BEFORE stack_ detach. Pre-mark bumps
  tag + flips state so any `W` mid-splice CAS expecting the old snap
  fails. On detach success, link_cas_state_certify atomically promotes
  ORPHAN → CLEAN AND publishes CERT. On detach failure the slot lingers
  mid-chain as SIGNALED_ORPHAN with CERT=0; walkers splice any
  SIGNALED_* like TIMED_OUT (publishing CERT via finalize CAS on splice
  success) but DO NOT reclaim (owner resets via clear_slot_owned).
- `A` (stack-steal) is a CERT publisher in BOTH outcomes of its
  per-slot link_cas_state_detached: success ⇒ `with_state_set_cert*`
  publishes atomically with the state CAS; failure-on-SIGNALED_*_ORPHAN
  ⇒ follow-up `link_cas_state_certify<ORPHAN, CLEAN>` publishes CERT
  to close the off-chain-without-cert window (the slot is structurally
  off-chain by the earlier stack_ CAS, but the failing per-slot CAS
  didn't set CERT itself). This makes "stack-steal removes ⇒ CERT
  published" total.
- `W` re-verifies reachability on each hop via the full 64-bit
  prev.link; mark-then-help on observed MARK (see §LINK_MARK_BIT) so
  no walker waits on another walker's continued execution.
- `R` direct freelist push. Safe under the state+tag fold: any W
  capturing this link pre-reclaim fails its mid-splice CAS on
  tag/state mismatch; any W reading post-reclaim observes IDLE on a
  reachable chain → Retry per the IDLE-on-chain protocol. Pool memory
  is statically allocated and never freed, so stale derefs read VALID
  memory.
- `reclaim_slot` bumps target.generation as part of `freelist_push`;
  `W` captures gen at entry and re-checks before any CAS.
- Reclaim is single-actor per lifecycle gen: TIMED_OUT ⇒
  `harris_unlink` returns true (caller reclaims); SIGNALED ⇒ returns
  false (woken owner reclaims via `clear_slot_owned`).

### Race-by-race

| Pair  | Resolution |
|-------|------------|
| W vs P | P's pre-mark bumps prev.link's tag+state before combined_ touch; W's captured snap fails mid-splice. |
| W vs A | A's stack steal empties head (W's head-path sees NULL, exits); per-slot CAS-with-snap fails any pending W mid-splice CAS; TIMED_OUT in stolen list → gen bump caught by W; SIGNALED_*_ORPHAN observed on per-slot CAS-fail (waker's pre-mark landed after A's snap) → A follows up with `link_cas_state_certify` to publish CERT, since the slot is off-chain by A's steal but the failing CAS didn't set CERT itself. |
| W vs T | T updates head only; W mid-splice unaffected; W head-splice retries on head change. |
| W vs W'| Distinct targets: independent. Same target: at most one wins parent CAS; the other observes the post-splice link or fails parent CAS, then either helps complete a marked target (if it observed MARK=1) or restarts. |
| W vs R | Tag+state fold on the slot's link makes any stale W snapshot fail its CAS; non-CAS reads observe IDLE-on-reachable-chain and Retry. |
| ABA    | target.gen is 32-bit (130+ years at 10K reclaims/sec). Per-link tag is 32-bit and bumped on every link write; per-slot wraps require >2^32 link writes within one walk's lifetime — physically impossible at any realistic clock rate. |

### Safety triad

The protocol's safety against stale walker references rests on three
structural properties:

1. **Tag monotonicity.** Every link write bumps the per-slot 32-bit
   tag. A walker holding a stale link snapshot fails any CAS attempt
   on tag mismatch.
2. **IDLE-on-reachable-chain Retry.** A walker that reads a chain-
   reachable slot's link and observes `state == IDLE` has caught a
   stale snapshot from a stack-stealer's reclaim. The walker re-reads
   from head (now NULL or a fresh top after the steal); progress
   bounded.
3. **Pool memory is never freed.** Slots are statically allocated in
   the demand-committed pool; pages get committed on first
   exhaustion and never decommitted. Any stale walker dereference
   reads VALID memory, never UAF — worst case is a tag-mismatch CAS
   failure or an IDLE-on-chain Retry, both bounded.

Together these eliminate the need for QSBR/hazard-pointer style
quiescence gating before reclaim. `reclaim_slot` direct-pushes onto
the freelist with no deferral.

### Splice-failure bail (load-bearing)

`link_finalize_after_splice` is a single strong CAS bail-on-fail (NO
retry loop). On CAS failure, another actor wrote past our snap.
Possibilities:

- Owner's `clear_slot_owned` slow path raced and wrote IDLE+CERT=1.
  CERT is published by owner; we have nothing to do.
- Owner cleared, reused, re-pushed onto a new chain, and a new waker
  pre-marked. CERT publish on the new chain is the new lifecycle's
  responsibility (its own walker/upgrade path).

**Retrying would risk certifying a slot now on-chain in a new
lifecycle.** Bail unconditionally; correctness is upheld by the
alternate publisher.

### IDLE on a reachable chain

Walkers that observe `state == IDLE` on a reachable chain hit the
legitimate "stale snapshot" signal — a stack-stealer (notify_all /
drain_waiters) emptied head, then `reclaim_slot`'s
`link_exchange_state_to_idle_certify` on a TIMED_OUT slot in the stolen
list left it at IDLE+CERT=1 with `.next` still pointing into stolen
successors. Walkers that captured prev/curr before the steal can land
here. Retry re-reads head (now NULL after the steal, or a fresh top), so
progress is bounded and no real chain corruption is implied.

**NEVER assert on IDLE-mid-walk.**

---

## §Reclaim authority

`ReclaimAuthority` is a typed proof of detach for the `reclaim_slot`
entry point. Constructible only via four named factories, each
documenting a distinct detach origin:

| Factory | Detach origin |
|---------|---------------|
| `after_walker_splice` | harris_unlink's parent CAS committed; includes inline-unlink (timed-out owner walks itself off) and opportunistic dead-intermediate splice during target walk. |
| `after_stack_pop` | single-entry stack_ CAS detach committed by pop_and_signal_one / handoff_one / drain_stale_top's TIMED_OUT branch. |
| `after_stack_steal` | whole-stack steal CAS committed by notify_all / notify_all_chunked / drain_waiters; every slot in the stolen list is provably off the live chain. |
| `after_unlinker_dispatch` | slot_cleanup (thread exit) or Phase 1.75 owner self-recovery dispatched a registered subsystem unlinker which returned true, or the slot had no chain residency to begin with (orphan with no subsystem owner). |

The token is move-only and carries the slot index it authorises for
reclaim. `reclaim_slot(ReclaimAuthority)` is the only entry point and
rejects any path that cannot produce one. Auditing reclaim sites is then
a localised review of the matching `after_*` factory call.

### Reclaim path

`reclaim_slot` is straight-line:

```cpp
if (link_exchange_state_to_idle_certify(pool[index].link) == IDLE)
    return;            // another reclaimer won the idempotency CAS
freelist_push(index);  // gen bump + subsystem clear + push
```

The early-bail-on-IDLE is load-bearing: it keeps `freelist_push`
single-actor, since double-push would corrupt the freelist Treiber
stack. Multiple actors may race into `reclaim_slot` (harris splice,
pop_and_signal_one, slot_cleanup, Phase 1.75 self-reclaim) — exactly
one wins the state CAS and proceeds to push.

`freelist_push` performs the full slot reset: `subsystem → None`,
`thread_id → 0`, `wait_address → 0`, `gen.fetch_add(1, RELEASE)`,
`link_store` to IDLE+CERT with the freelist-head as next, then the
freelist-head CAS to publish.

A walker that captures `target.gen` between the state CAS in
`reclaim_slot` and `freelist_push`'s gen bump observes
IDLE-on-reachable-chain (Retry) or fails its mid-splice CAS on tag
mismatch; both outcomes converge on a gen re-check after the push
completes, by which point gen has bumped.

---

## §WaitCondition + PredicateFn

Unified waiter descriptor with two shapes, one struct:

- **Classic** — `pred = nullptr`, `arg = expected-value`. Exit: `v != arg`.
  Waker-side filter is absent (`slot.filter_fn` left as nullptr).
- **Predicated** — `pred = user PredicateFn`, `arg = user-supplied`. Exit:
  `pred(v, arg)`. The same function is installed on `slot.filter_fn` so
  the waker agrees.

`PredicateFn` signature:
`bool (*)(uint32_t current_value, uint32_t arg) noexcept`. The `noexcept`
is part of the type — passing a non-noexcept function triggers a compile
error at the registration site.

### Waker filter contract (invariant I7)

The same function serves two roles:

- **Waiter-side stop condition** — checked in `wait_impl` fast path,
  Phase 1 hardware spin (via `spin_on_pred`), Phase 2 CAS-push loop,
  Phase 4 stale-alert branch, Phase 4 in-kernel re-park branch, and the
  top-level absorb wrapper. `ret == 0` guarantees `pred(load(), arg)`
  held at some point in the wait's lifetime.

- **Waker-side filter** — `pop_and_signal_one` /
  `signal_first_match_after` read `slot.filter_fn` concurrently with the
  lock holder's critical path and skip waiters whose predicate is FALSE
  on current `value_`, walking to the next eligible waiter.

Caller notify contract (LOAD-BEARING): every transition that flips the
predicate FALSE → TRUE in `value_` MUST call `notify_one` / `notify_all`
/ `unlock_notify`, else the wake is lost — there is no eventual re-check
fallback. Same contract as classical `wait()` (pred = `v != expected`),
generalised.

`fn` must be either nullptr or a freestanding function with
process-lifetime validity (waker reads concurrently with slot reuse).
`arg` is a plain u32 with no dereferenceable interpretation. Must NOT be
reinterpreted as a pointer; the value-type restriction is what makes
torn reads of (fn, arg) during a concurrent slot-reuse cycle tolerable.

`fn` must be noexcept, side-effect-free, deterministic, must not take
locks, allocate, or recurse into any wait primitive. The waker invokes
it on the lock holder's critical path — any blocking is a deadlock.

### Filter read protocol — waker side (concurrent with slot reuse)

Wakers gate the filter read on `state == WAITING || IN_KERNEL` —
chain residency is the precondition. IDLE-state slots are unreachable
to wakers; their filter is dormant.

For chain-resident slots, the read sequence:

1. Load `slot.generation` (ACQUIRE) → `gen_pre`
2. Load `filter_fn` (RELAXED)
3. Load `filter_arg` (RELAXED)
4. Re-load `slot.generation` (ACQUIRE) → `gen_post`
5. If `gen_pre != gen_post`, treat as "no filter for this cycle" (the
   pair may be torn). Fall through to unfiltered wake — the pre-mark
   CAS with stale snap will fail and force a retry, so there is no
   correctness hole on the torn-read path.
6. Otherwise (fn, arg) is consistent. If `fn == nullptr`, no filter;
   proceed with unfiltered wake. Else call `fn(value_.load(ACQUIRE),
   arg)`; false ⇒ skip, true ⇒ wake.

Owner-side write contract: Phase 2 push setup unconditionally writes
`filter_fn` and `filter_arg` on every wait entry — HasPredicate=true
stores caller's pred/arg, HasPredicate=false stores nullptr/0. The
slot's filter at chain-publish time (the moment the combined_ CAS
publishes the slot onto the stack) always reflects the current wait.
`clear_slot_owned` does NOT clear these fields (see §clear_slot_owned
ordering — IDLE-state filter is dormant).

`handoff_one` and `notify_all` IGNORE the predicate — `HANDOFF_BIT` is
unconditional transfer; broadcast opted into fan-out.

---

## §UnlockOutcome (handoff_one tri-state)

| Outcome | Caller obligation |
|---------|-------------------|
| `Empty` | No waiter woken; caller MUST store unlock_val AND pop_and_signal_one (Dekker catch for any waiter that pushed after handoff_one's scan). |
| `Completed` | Entire unlock sequence is done: unlock_val has been stored inside handoff_one and the target waiter has been notified. Caller MUST NOT store and MUST NOT pop. Covers both the IN_KERNEL wake path AND the WAITING-branch waiter-self-committed fallback. |
| `Handoff` | Ownership transferred via `HANDOFF_BIT` on a WAITING target; value stays at the locked sentinel; caller MUST NOT store and MUST NOT pop. |

The Completed/Handoff split drives the critical ordering fix: for
Completed, `handoff_one` is responsible for sequencing the value_ store
BEFORE any wake (cache-line wake_word publish or alert syscall). This
closes the hang where a waker alerted an IN_KERNEL waiter, was preempted
before value_.store, and the alerted waiter re-CAS'd the still-LOCKED
value and re-parked.

---

## §Pool invariants

| Invariant | Statement |
|-----------|-----------|
| **I1** | `LINK_CERT_BIT` (slot.link bit 49) ⇒ slot is provably off-chain. See §LINK_CERT_BIT. |
| **I2** | Wakers pre-mark link.state via `link_cas_snap` — a single atomic publishing wake + wake-kind + cleanup-responsibility + tag bump. |
| **I4** | Wakers capture `slot.thread_id` AND `owner_ref_of(slot)` TOGETHER before the alert syscall. See §Capture-tid-with-owner-ref. |
| **I5** | Owners exit Phase 4 via exactly one of: (a) `state_is_signaled(st) → decode_signaled_ret`; (b) `cancel_or_absorb(...) → unified CAS + decode-on-race`; (c) reloop (spurious) → refresh entry_epoch. |
| **I7** | `slot.filter_fn` is the same function as the waiter's predicate; caller notify contract binds them. See §WaitCondition. |

---

## §clear_slot_owned ordering

Load-bearing order:

1. `gen++` RELEASE — single barrier; remaining stores RELAXED.
2. TLS gen refresh so owner's fast-path reuse compare matches.
3. `subsystem → None` BEFORE (4) so racing thread-exit cleanup, which
   dispatches on subsystem, skips us.
4. `IDLE+CERT` publish — fast or slow path; both establish the off-chain
   certificate and IDLE state, dropping MARK.
5. `wait_address` cleared (invalidation gate for racing wakers).

Fast-path probe: if CERT=1 and MARK=0 at entry, no walker is mid-fold on
this slot and no upstream CERT publish is outstanding — the slot is
owner-exclusive off-chain. Plain release-store of IDLE+CERT=1 with
bumped tag is sufficient and race-free. Saves one LOCK-prefixed RMW per
wait completion in the steady state.

Slow path (CAS loop) covers the `[t0, t1]` window where a racing walker
fold or upstream certify CAS could otherwise clobber our CERT publish
(its CAS observes our updated value and either fails on state mismatch
or preserves CERT=1).

Both paths land at IDLE+CERT=1+MARK=0 with tag bumped — same terminal.

### `filter_fn` / `filter_arg` are NOT cleared

Wakers only read these from chain-resident slots
(`state == WAITING || IN_KERNEL` — checked in `pop_and_signal_one`
and `signal_first_match_after` before any filter load). An IDLE
slot's filter is dormant; no waker can reach it. Phase 2 setup
unconditionally rewrites both fields on every wait entry —
HasPredicate=true stores caller's pred/arg, HasPredicate=false stores
nullptr/0 — so the slot's filter at chain-publish time always
reflects the current wait. Clearing them in `clear_slot_owned` would
be redundant work the next Phase 2 setup overwrites.

---

## §`operator=` is deleted

`futex = v;` was a split-pattern footgun — it emits a RELEASE store, and
a subsequent `notify_one()`/`notify_all()` deadlocks on x86 due to
StoreLoad reordering between the MOV and the notify's ACQUIRE load of
stack_.

Use `store_and_notify(v)` or `store_and_notify_all(v)` for the wake
protocol (both SEQ_CST, full barrier). For RMW-driven wakes, call the
RMW (compare_exchange / exchange / fetch_*) directly — all
LOCK-prefixed, so the subsequent notify is safe.

---

## §Software-pipelined wake walk

`notify_all`, `notify_all_chunked`, and `drain_waiters` use a
PIPELINE_DEPTH=4 software pipeline to decouple the ACQUIRE load
(cold-line DRAM/LLC miss) from the LOCK XCHG (x86 pipeline fence). Four
loads in flight via MSHRs, overlapping cold-line latency across
iterations rather than stacking behind each XCHG fence. Depth=4 covers
~200–250 ns DRAM miss at ~50–80 ns per-slot; deeper bloats the ring
with no added benefit.

Filter policy: broadcast IGNORES `slot.filter_fn` by design. `notify_all`'s
contract is "wake everyone" — the caller already opted into fan-out.
Waiters whose user-level condition doesn't match re-check in their caller
loop and re-park via the Phase 0 fast path. Applying the filter here
would break existing condvar-broadcast and fork-reinit callers that
assume every waiter gets woken regardless of the futex's value.

---

## §Fork

`reset_for_fork` atomically zeroes `value_` + `stack_` WITHOUT draining.
After fork, stale waiter slots reference parent-process threads —
`drain_waiters()` would send `NtAlertThreadByThreadId` to the parent
(TIDs are system-wide on Windows), causing spurious wakes there. The
child has no threads to wake; clearing the stack is enough. Stale slot
cleanup happens lazily if the child later encounters one via the
pop-side `wait_address` check.

---

## §AutoBoost correlation

`PS_ALERT_THREAD_EXTENDED_PARAMETER.Pointer` is set to the wait-key
address (`this` for a Futex; the user futex address for a parking-lot
slot) on every `alert_multiple` call. Consistent with NT's AutoBoost
convention — the wait key correlates the alert with the resource the
waiter is parked on, so the kernel's priority-boost heuristic can run.
