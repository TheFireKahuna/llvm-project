//===--- Thread-local word with hardware-monitor sleep -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ThreadLocalWord — 64-byte cache-line-aligned word optimized for asymmetric
// access: zero-cost owner-thread reads, cheap cross-thread signals.
//
// Designed as the thread-local analogue of the Futex class, transferring all
// applicable Futex optimizations to a single-owner primitive:
//
//   Futex opt #2  — UMWAIT/MWAITX hardware address monitor.
//     monitor_sleep() arms a cache-line monitor on &value_ and enters C0.2
//     (Intel WAITPKG) or MWAITX (AMD Zen+). Any store to the value's cache
//     line — from any core — wakes the owner with near-zero power draw and
//     near-zero SMT resource consumption. This is the primary sleep path.
//
//   Futex opt #4  — Split-width access.
//     Owner reads are plain loads (MOV on x86, LDR on AArch64). Cross-thread
//     writes use __atomic_store_n / __atomic_fetch_or (RELEASE). The owner
//     never executes an atomic prefix on the read path.
//
//   Futex opt #5  — Implicit cache-line wake.
//     When the owner is in UMWAIT/MWAITX, a cross-thread store to value_
//     writes the cache line, which IS the wake signal. No explicit
//     NtAlertThreadByThreadId is needed — the hardware coherence protocol
//     delivers the wake. Explicit alerts are only needed when the owner has
//     fallen through to kernel sleep (NtWaitForAlertByThreadId).
//
//   Futex opt #6  — KUSER_SHARED_DATA spin budget.
//     The monitor sleep budget comes from the same kernel-calibrated threshold
//     at 0x7FFE036A that ntdll and the Futex use. No hardcoded spin counts.
//
//   Futex opt #7  — TSC-bounded sleep.
//     UMWAIT/MWAITX deadlines are expressed in TSC ticks, not iteration
//     counts. Behavior is consistent regardless of IPC or cache miss rates.
//     Budget: threshold × 2048 ticks (longer than Futex's 1024× because
//     single-owner has no Treiber push overhead to amortize, and catching
//     the cross-thread write in user-mode avoids the kernel sleep path).
//
//   Futex opt #8  — Cache-line alignment.
//     The entire struct is alignas(64). A cross-thread write to value_ does
//     not false-share with unrelated data. The owner's monitor watches one
//     clean cache line.
//
//   Futex opt #9  — Generation counter.
//     Every cross-thread signal() bumps generation_. The owner can snapshot
//     the generation, do work, then check changed_since(snapshot) to detect
//     "any cross-thread write during this window" without reading value_.
//     Useful for epoch-based "is my cached state still valid?" checks.
//
//   Futex opt #13 — Pending-flags bitmask.
//     value_ can serve as a bitmask of pending conditions. The owner checks
//     any_pending() — a single MOV + TEST + predicted-not-taken Jcc. Cross-
//     thread writers set specific bits via signal_or(). The owner clears
//     handled bits via clear_flags() (atomic AND NOT, cold path only).
//
//   Futex opt #14 — Compile-time Interruptible template.
//     wait_for_change<Interruptible=false>() eliminates all interrupt-
//     handling codegen via if-constexpr. The non-interruptible path has
//     zero branch predictor pollution from APC/signal handling.
//
//   Futex opt #15 — Alert drain after kernel wake.
//     After exiting kernel sleep, a zero-timeout NtWaitForAlertByThreadId
//     consumes any stale alert token left by a race between the owner's
//     park_state clear and a cross-thread writer's alert. Without this,
//     the stale token would cause a ghost wake on the owner's next kernel
//     wait (Futex, IoRing, etc.).
//
// Memory layout (64 bytes, one cache line):
//
//   [0:4)   value_       — user-visible word (flags / state / counter)
//   [4:8)   generation_  — bumped on every cross-thread store
//   [8:12)  owner_tid_   — NT thread ID for kernel alert
//   [12:13) park_state_  — NOT_PARKED / KERNEL_PARKED
//   [13:16) reserved
//   [16:64) padding
//
// Owner hot path (poll or read):
//   read():        RELAXED load → 1 cycle (L1 hit, always resident)
//   any_pending(): RELAXED load + TEST + Jcc → 1-2 cycles
//   test_flags(m): RELAXED load + AND + TEST + Jcc → 2-3 cycles
//   (RELAXED = plain MOV on x86, plain LDR on AArch64 — compiler barrier only)
//
// Owner blocking path:
//   wait_for_change(expected):
//     Phase 1: UMWAIT/MWAITX on &value_ (TSC-bounded)
//     Phase 2: NtWaitForAlertByThreadId (kernel sleep)
//     No Treiber stack, no WaitSlot, no CAS push — one waiter max.
//
// Cross-thread path:
//   signal(target, value): atomic store + generation bump + conditional alert
//   signal_or(target, bits): atomic OR + generation bump + conditional alert
//   store(target, value): atomic store + generation bump (no alert)
//
// Correctness properties:
//   - No lost wakes: Dekker protocol on park_state_ / value_ ensures either
//     the owner sees the new value before sleeping, or the writer fires an
//     alert after the owner enters kernel sleep. The owner uses SEQ_CST
//     store for park_state_ (XCHG on x86, flushing the store buffer);
//     the writer uses SEQ_CST generation bump (LOCK XADD on x86) as
//     the Dekker fence between value store and park_state_ load.
//   - No ghost wakes: alert drain on exit consumes stale tokens.
//   - Fork safe: fork_reinit() updates owner_tid_ and clears park state.
//   - Signal-handler safe for polling: read(), any_pending(), test_flags(),
//     clear_flags() are all safe from signal handlers and APCs.
//   - wait_for_change() is NOT reentrant: a signal handler must not call
//     wait_for_change() on the same ThreadLocalWord while the owner is
//     already in wait_for_change(). The kernel sleep phase uses park_state_
//     as shared state that cannot nest. Signal handlers should poll with
//     read()/any_pending(), never block with wait_for_change().
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LOCAL_WORD_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LOCAL_WORD_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/time/abs_timeout.h"

namespace LIBC_NAMESPACE_DECL {

struct alignas(64) ThreadLocalWord {
  // ---------------------------------------------------------------------------
  // Layout fields — one cache line (64 bytes)
  // ---------------------------------------------------------------------------
  //
  // value_ and generation_ occupy the first 8 bytes. A single cache-line fetch
  // loads both. park_state_ is on the same line — a cross-thread write to
  // value_ invalidates the entire line, which is correct: the owner's next
  // load of park_state_ (to clear it on wake) will pull the line anyway.

  // User-visible word. Owner reads with plain load, cross-thread writes with
  // atomic store. Can be used as flags bitmask, state enum, or counter.
  uint32_t value_;

  // Cross-thread write generation. Bumped by every signal()/signal_or()/
  // store(). Owner snapshots it to detect "any change since I last checked"
  // without reading value_ itself. [Futex optimization #9]
  uint32_t generation_;

  // Owner's NT thread ID. Set once at init() from TEB.ClientId.UniqueThread.
  // Read by cross-thread writers to fire NtAlertThreadByThreadId when the
  // owner is kernel-parked. Stable for the thread's lifetime.
  uint32_t owner_tid_;

  // Park state machine. Written by owner before entering kernel wait, read
  // by cross-thread writers to decide if an explicit NtAlert is needed.
  //
  //   NOT_PARKED (0) — Owner is running, polling, or in UMWAIT/MWAITX.
  //     In all three cases, a cross-thread store to value_ is sufficient:
  //     - Running/polling: owner will see the new value on next read().
  //     - UMWAIT/MWAITX: cache-line write wakes the hardware monitor.
  //     No kernel alert needed. [Futex optimization #5: implicit wake]
  //
  //   KERNEL_PARKED (1) — Owner is in NtWaitForAlertByThreadId. The only
  //     way to wake it is NtAlertThreadByThreadId(owner_tid_). Cross-thread
  //     writer MUST fire the alert.
  static constexpr uint8_t NOT_PARKED = 0;
  static constexpr uint8_t KERNEL_PARKED = 1;

  // Zero-timeout for alert drain: -1 = 100ns relative (essentially immediate).
  // Used on all exit paths from wait_for_change() to consume stale tokens.
  static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};

  cpp::Atomic<uint8_t> park_state_;

  uint8_t reserved_[3];
  char pad_[48];

  // ---------------------------------------------------------------------------
  // Initialization
  // ---------------------------------------------------------------------------

  /// Initialize for the calling thread. Must be called once by the owner
  /// before any other operation. Sets owner_tid_ from TEB.ClientId.
  ///
  /// value_ is NOT reset — it was already zero-initialized by the containing
  /// struct's zero_lifecycle() / memset, and cross-thread writers (e.g.,
  /// cancel::request via signal_or) may have set bits between the parent's
  /// registry_register() and this init() call. Overwriting value_ here
  /// would lose those notifications.
  LIBC_INLINE void init() {
    // value_ deliberately left as-is — see comment above.
    generation_ = 0;
    owner_tid_ = NtCurrentThreadId();
    park_state_.val = NOT_PARKED;
    __builtin_memset(reserved_, 0, sizeof(reserved_));
    __builtin_memset(pad_, 0, sizeof(pad_));
  }

  /// Non-atomic full reset. Safe only when no concurrent access is possible
  /// (e.g., construction, single-threaded reinit).
  LIBC_INLINE void reset(uint32_t val = 0) {
    value_ = val;
    generation_ = 0;
    park_state_.val = NOT_PARKED;
  }

  /// Reset after fork. The surviving thread's word is valid; update the TID
  /// (child process gets new TIDs) and clear park state. Dead threads' words
  /// are orphaned — bounded cost, same as ThreadScratch fork semantics.
  LIBC_INLINE void fork_reinit() {
    owner_tid_ = NtCurrentThreadId();
    park_state_.val = NOT_PARKED;
  }

  // ===================================================================
  // Owner-thread API — hot path, zero atomics
  // ===================================================================
  //
  // All owner reads use __atomic_load_n(RELAXED) which generates the same
  // instruction as a plain load (MOV on x86, LDR on AArch64) but acts as
  // a compiler optimization barrier — preventing LICM hoisting, CSE merging,
  // or register caching across calls. Zero runtime cost, safe for polling.
  //
  // [Futex optimization #4: split-width — owner plain loads]

  /// Read the value word. One instruction, ~1 cycle (L1 hit when no
  /// cross-thread write since last read). After a cross-thread write,
  /// first read pulls the line from the writer's L1 (~40-80ns same socket).
  ///
  /// Uses __atomic_load_n(RELAXED) instead of a plain field read to prevent
  /// the compiler from hoisting the load out of loops, merging consecutive
  /// reads, or caching the value in a register across calls. On x86 this
  /// generates the same MOV instruction as a plain read. On AArch64, the
  /// same LDR. The only effect is a compiler optimization barrier — zero
  /// runtime cost.
  LIBC_INLINE uint32_t read() const {
    return __atomic_load_n(&value_, __ATOMIC_RELAXED);
  }

  /// Write the value word. Owner-only, no cross-thread fence.
  /// Uses RELAXED atomic to avoid formal UB if a cross-thread signal_or()
  /// races (same codegen as plain store on x86/AArch64).
  /// WARNING: Overwrites the entire word. Do not mix with signal_or() —
  /// a concurrent signal_or() that sets flag bits may be silently lost.
  /// Use clear_flags() instead when cross-thread flag writers are active.
  LIBC_INLINE void write(uint32_t val) {
    __atomic_store_n(&value_, val, __ATOMIC_RELAXED);
  }

  /// Test flag bits. Compiles to: MOV + TEST + Jcc.
  /// [Futex optimization #13: pending-flags fast path]
  LIBC_INLINE bool test_flags(uint32_t mask) const {
    return (__atomic_load_n(&value_, __ATOMIC_RELAXED) & mask) != 0;
  }

  /// Test if any flags are set (value != 0). Even cheaper: MOV + TEST reg,reg.
  /// Designed for the hot-path guard in allocators:
  ///   if (LIBC_UNLIKELY(word.any_pending())) { /* handle */ }
  /// [Futex optimization #13]
  LIBC_INLINE bool any_pending() const {
    return __atomic_load_n(&value_, __ATOMIC_RELAXED) != 0;
  }

  /// Clear specific flag bits. Atomic AND NOT because a cross-thread writer
  /// might set other bits concurrently. Cold path — only called when flags
  /// are actually set.
  ///
  /// Ordering: ACQ_REL. Callers (e.g. dispatch_engine's re-check gate) rely
  /// on the clear being globally visible before subsequent loads, and on
  /// prior stores being visible before the clear. On x86 this is a
  /// LOCK-prefixed AND — a full barrier regardless of order, so ACQ_REL is
  /// free. On AArch64 the AQ+RL tags prevent the compiler/CPU from hoisting
  /// later ACQUIRE loads (e.g. of pending bitmaps) above the clear, closing
  /// a previously-latent re-check race that required an external SEQ_CST
  /// fence to paper over. Cold path — no hot-path cost.
  LIBC_INLINE void clear_flags(uint32_t mask) {
    __atomic_fetch_and(&value_, ~mask, __ATOMIC_ACQ_REL);
  }

  /// Snapshot the generation counter for later changed_since() check.
  /// Uses RELAXED load — same compiler-barrier rationale as read().
  LIBC_INLINE uint32_t snapshot_gen() const {
    return __atomic_load_n(&generation_, __ATOMIC_RELAXED);
  }

  /// True if any cross-thread signal()/signal_or()/store() occurred since
  /// the snapshot was taken.
  /// [Futex optimization #9: generation tagging]
  LIBC_INLINE bool changed_since(uint32_t snapshot) const {
    return __atomic_load_n(&generation_, __ATOMIC_RELAXED) != snapshot;
  }

  // ===================================================================
  // Owner-thread blocking — sleep until value changes
  // ===================================================================
  //
  // Two-phase adaptive wait. No Treiber stack, no WaitSlot — single owner
  // means at most one waiter. The park_state_ flag is the only coordination.
  //
  // Phase 1: UMWAIT/MWAITX hardware address monitor on &value_.
  //   [Futex optimizations #2, #5, #6, #7]
  //   - Arms cache-line monitor on value_
  //   - Checks value (if changed, return — no wasted sleep)
  //   - Enters C0.2/MWAITX sleep (TSC-bounded)
  //   - Wakes on: cache-line write, TSC expiry, or interrupt
  //   Budget: threshold × 2048 TSC ticks (~7.3µs at 4.5GHz, threshold=16)
  //
  // Phase 2: NtWaitForAlertByThreadId kernel sleep.
  //   [Futex optimizations #14, #15]
  //   - Sets park_state_ = KERNEL_PARKED (Dekker publish)
  //   - Re-checks value (if changed, clear park, return)
  //   - Enters kernel wait
  //   - On wake: clear park, drain stale alert tokens
  //
  // Contrast with Futex: no Phase 2 (Treiber push), no Phase 2.5 (slot spin),
  // no Phase 3 (WAITING→IN_KERNEL transition). Single owner eliminates all
  // multi-waiter coordination.

  using Timeout = internal::AbsTimeout;

  /// Sleep until value_ != expected, or timeout expires.
  ///
  /// Returns:
  ///    0         — value changed
  ///   -ETIMEDOUT — timeout expired before value changed
  ///   -EINTR     — (Interruptible only) APC/signal interrupt
  ///
  /// [Futex optimization #14: compile-time Interruptible template]
  template <bool Interruptible = false>
  LIBC_INLINE long
  wait_for_change(uint32_t expected,
                  cpp::optional<Timeout> timeout = cpp::nullopt) {

    // park_state_ is per-instance single-owner state; nesting (e.g. a
    // signal handler or VEH filter calling wait_for_change() on the same
    // ThreadLocalWord while the owner is already parked in Phase 2) would
    // silently clobber the outer call's park state and lose its wake.
    // This assert catches the KERNEL_PARKED case deterministically; the
    // "re-enter during Phase 1" subcase can't clobber because Phase 1
    // only uses the hardware monitor. See the non-reentrancy note at the
    // top of this file.
    LIBC_ASSERT(park_state_.load(cpp::MemoryOrder::RELAXED) != KERNEL_PARKED);

    // Fast path: value already changed.
    if (read() != expected)
      return 0;

    // --- Phase 1: Hardware address monitor ---
    //
    // [Futex opt #2: UMWAIT/MWAITX]
    // [Futex opt #5: implicit cache-line wake]
    // [Futex opt #6: KUSER_SHARED_DATA budget]
    // [Futex opt #7: TSC-bounded sleep]
    if (monitor_sleep(expected))
      return 0;

    // Check for already-expired timeout before entering kernel.
    if (timeout) {
      LARGE_INTEGER probe;
      LARGE_INTEGER *p = timeout_to_nt(timeout, probe);
      if (p && p->QuadPart == -1)
        return -ETIMEDOUT;
    }

    // --- Phase 2: Kernel sleep with Dekker protocol ---
    //
    // The Dekker ordering ensures no lost wakes:
    //
    //   Owner:  store(KERNEL_PARKED, SEQ_CST) → load(value_, ACQUIRE)
    //   Writer: store(value_, RELEASE) → gen++(SEQ_CST) → load(park_state_, ACQUIRE)
    //
    //   Case A: Owner's load sees new value → don't sleep, return 0.
    //   Case B: Writer's load sees KERNEL_PARKED → fires NtAlert, owner wakes.
    //   Case C: Writer's load sees NOT_PARKED → but the value store happened
    //           first, so owner's load-after-park will see it (reduces to A).
    //
    //   On x86, RELEASE store = plain MOV (no barrier). The store buffer can
    //   delay the park_state_ write past the value_ read — breaking Dekker.
    //   SEQ_CST store = XCHG on x86, which flushes the store buffer before
    //   proceeding to the load. On AArch64, stlr is the same for RELEASE
    //   and SEQ_CST in Clang codegen — zero additional cost.
    //
    //   The parking lot (futex_addr.h) uses an explicit
    //   __atomic_thread_fence(SEQ_CST) for the same reason (line 472).
    //   The embedded Futex avoids Dekker entirely via CAS-64 fusion.
    park_state_.store(KERNEL_PARKED, cpp::MemoryOrder::SEQ_CST);

    // Dekker re-check: value may have changed while setting park_state_.
    // Use __atomic_load_n to prevent the compiler from reusing a stale
    // register-cached value from before the park_state_ store.
    if (__atomic_load_n(&value_, __ATOMIC_ACQUIRE) != expected) {
      park_state_.store(NOT_PARKED, cpp::MemoryOrder::RELAXED);
      // Drain: a cross-thread writer may have loaded park_state_ between
      // our SEQ_CST store of KERNEL_PARKED and our RELAXED store of
      // NOT_PARKED. If so, it fired a stale NtAlertThreadByThreadId.
      // Consume the token to prevent ghost wakes on the owner's next
      // kernel wait (Futex, IoRing, etc.). Best-effort: catches the
      // common timing; a late-arriving alert from a concurrent writer
      // is handled as a spurious wake by the next wait call.
      {
        PVOID tid_cookie =
            reinterpret_cast<PVOID>(static_cast<uintptr_t>(owner_tid_));
        ::NtWaitForAlertByThreadId(tid_cookie, &DRAIN_TIMEOUT);
      }
      return 0;
    }

    LARGE_INTEGER nt_timeout;
    LARGE_INTEGER *nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);

    // Use owner TID as the wait address cookie (matches Futex convention).
    PVOID tid_cookie =
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(owner_tid_));

    long ret = 0;
    for (;;) {
      NTSTATUS status = ::NtWaitForAlertByThreadId(tid_cookie, nt_timeout_ptr);

      // Value changed — success. Use __atomic_load_n for the Dekker re-read
      // (must see the cross-thread store that triggered the alert).
      if (__atomic_load_n(&value_, __ATOMIC_ACQUIRE) != expected) {
        ret = 0;
        break;
      }

      if (status == STATUS_TIMEOUT) {
        ret = -ETIMEDOUT;
        break;
      }

      // [Futex optimization #14: compile-time Interruptible]
      if constexpr (Interruptible) {
        // APC or signal handler woke us. Return -EINTR so the caller can
        // check for pending signals and restart or abort.
        if (status != STATUS_ALERTED) {
          ret = -EINTR;
          break;
        }
      }

      // Non-interruptible: absorb spurious wake (stale alert, APC) and retry.
      // Recompute timeout — time has passed.
      nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
      if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
        ret = -ETIMEDOUT;
        break;
      }
    }

    park_state_.store(NOT_PARKED, cpp::MemoryOrder::RELEASE);

    // [Futex optimization #15: Alert drain]
    //
    // Drain any stale alert token left by a race between our park_state_
    // clear and a cross-thread writer's NtAlertThreadByThreadId. Without
    // this, the stale token would cause a ghost wake on the owner's next
    // NtWaitForAlertByThreadId call (from a Futex wait, IoRing sleep, etc.).
    //
    // Race scenario:
    //   1. Owner in kernel wait, park_state_ = KERNEL_PARKED
    //   2. Owner wakes (timeout or value change), clears park_state_
    //   3. Cross-thread writer reads park_state_ = KERNEL_PARKED (stale)
    //   4. Writer fires NtAlertThreadByThreadId — alert token queued
    //   5. Owner's next kernel wait returns immediately (ghost wake)
    //
    // The drain is unconditional: a zero-timeout NtWaitForAlertByThreadId
    // that finds no pending alert returns STATUS_TIMEOUT in ~200ns. This
    // is the cold exit path, so the cost is acceptable.
    { ::NtWaitForAlertByThreadId(tid_cookie, &DRAIN_TIMEOUT); }

    return ret;
  }

  // ===================================================================
  // Owner-thread blocking — sleep until external address changes
  // ===================================================================
  //
  // Zero-bounce adaptive wait for changes to an external address. The
  // cross-thread wake path (alert_if_parked) never writes to the owner's
  // cache line — it only reads park_state_ and conditionally fires an
  // NtAlert. This eliminates all write-invalidation traffic between the
  // reactor and the owner.
  //
  // Designed for IoRing CQE wait where cq->Tail is kernel-written and
  // the reactor provides a guaranteed NtAlert wake when the owner is
  // kernel-parked.
  //
  // Phase 0: UMWAIT/MWAITX on ext_addr (the primary data source).
  //   Budget: threshold × 2048 TSC ticks (full TLW budget — this is
  //   the only hardware monitor phase).
  //   Catches: kernel cq->Tail write in hardware sleep, zero reactor
  //   latency. For NVMe (5-20µs) this is the expected wake path.
  //
  // Phase 1: NtWaitForAlertByThreadId with single-address Dekker.
  //   park_state_ = KERNEL_PARKED (SEQ_CST) → re-check ext_addr →
  //   kernel sleep. The cross-thread alert_if_parked() is the Dekker
  //   partner — it reads park_state_ and fires NtAlert if KERNEL_PARKED.
  //
  // Correctness:
  //   The reactor doesn't store to value_ or any owner-side address.
  //   The Dekker is one-sided: the owner publishes KERNEL_PARKED, and
  //   the reactor detects it. This is sufficient because:
  //
  //   The reactor fires AFTER the kernel wrote ext_addr and signaled
  //   the completion event. Temporal ordering guarantees:
  //
  //   Case A: Owner re-checks ext_addr → changed → return 0.
  //     The kernel wrote ext_addr before/during the park setup.
  //
  //   Case B: Owner re-checks ext_addr → unchanged → NtWait.
  //     The kernel writes ext_addr later → signals event → reactor
  //     fires → reads park_state_ (KERNEL_PARKED) → NtAlert → wake.
  //     The owner's SEQ_CST store of KERNEL_PARKED is globally visible
  //     (XCHG on x86) long before the reactor runs (µs later).
  //
  //   Case C: Reactor fires before owner parks.
  //     Reactor sees NOT_PARKED → skips NtAlert. But the kernel already
  //     wrote ext_addr (event delivery is after the write), so the
  //     owner's Dekker re-check finds ext_addr changed → return 0.
  //
  //   Case D: Reactor fires between park and re-check.
  //     Reactor sees KERNEL_PARKED → fires NtAlert → token queued.
  //     Owner re-checks ext_addr → changed → return 0 → drains token.
  //     OR: re-check finds unchanged → NtWait → immediate return
  //     (queued token consumed).
  //
  // No lost wakes in any interleaving. The ext_addr re-check is the
  // critical safety net — it catches the common races. NtAlert handles
  // the rest.
  //
  // Cache-line traffic:
  //   - Owner's cq_word line: Modified throughout Phase 0. Transitions
  //     to Shared when reactor reads park_state_ (one snoop, ~20-40ns).
  //     Back to Modified when owner clears park_state_ (local upgrade).
  //   - No write-invalidation from reactor → owner. Zero bounce on the
  //     owner's hot cache line.

  /// Sleep until ext_addr != ext_expected, or timeout expires.
  ///
  /// The cross-thread wake path is alert_if_parked() — a read-only check
  /// of park_state_ plus conditional NtAlert. No writes to value_ or any
  /// field on the owner's cache line. This is the zero-bounce variant.
  ///
  /// Returns:
  ///    0         — external address changed (or NtAlert received)
  ///   -ETIMEDOUT — timeout expired
  ///   -EINTR     — (Interruptible only) APC/signal interrupt
  template <bool Interruptible = false>
  LIBC_INLINE long
  wait_for_addr(const volatile uint32_t *ext_addr, uint32_t ext_expected,
                cpp::optional<Timeout> timeout = cpp::nullopt) {

    // Fast path: external address already changed.
    if (__atomic_load_n(ext_addr, __ATOMIC_ACQUIRE) != ext_expected)
      return 0;

    // --- Phase 0: Hardware address monitor on external address ---
    //
    // Monitor the primary data source directly. For IoRing, this is the
    // kernel-mapped CQ tail — the kernel writes it when posting a CQE,
    // and the cache-line invalidation wakes the hardware monitor with
    // near-zero latency, bypassing the reactor bridge entirely.
    //
    // Budget: threshold × 2048 (full TLW budget). This is the only HW
    // monitor phase — no secondary value_ phase — so the full budget
    // is allocated here.
    if (monitor_sleep_on(ext_addr, ext_expected))
      return 0;

    // Check for already-expired timeout before entering kernel.
    if (timeout) {
      LARGE_INTEGER probe;
      LARGE_INTEGER *p = timeout_to_nt(timeout, probe);
      if (p && p->QuadPart == -1)
        return -ETIMEDOUT;
    }

    // --- Phase 1: Kernel sleep with single-address Dekker ---
    //
    // The SEQ_CST store of KERNEL_PARKED flushes the store buffer (XCHG
    // on x86), ensuring the park_state_ write is globally visible before
    // the ACQUIRE load of ext_addr. The cross-thread alert_if_parked()
    // reads park_state_ with ACQUIRE — the temporal ordering (reactor
    // runs after IOCP delivery, which is after the kernel wrote ext_addr)
    // ensures the owner's KERNEL_PARKED is visible if set.
    park_state_.store(KERNEL_PARKED, cpp::MemoryOrder::SEQ_CST);

    // Dekker re-check: the kernel may have written ext_addr while we
    // were setting park_state_. One ACQUIRE load — the ground truth.
    if (__atomic_load_n(ext_addr, __ATOMIC_ACQUIRE) != ext_expected) {
      park_state_.store(NOT_PARKED, cpp::MemoryOrder::RELAXED);
      {
        PVOID tid_cookie =
            reinterpret_cast<PVOID>(static_cast<uintptr_t>(owner_tid_));
        ::NtWaitForAlertByThreadId(tid_cookie, &DRAIN_TIMEOUT);
      }
      return 0;
    }

    LARGE_INTEGER nt_timeout;
    LARGE_INTEGER *nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);

    PVOID tid_cookie =
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(owner_tid_));

    long ret = 0;
    for (;;) {
      NTSTATUS status = ::NtWaitForAlertByThreadId(tid_cookie, nt_timeout_ptr);

      // Wake check: ext_addr changed → CQE available.
      if (__atomic_load_n(ext_addr, __ATOMIC_ACQUIRE) != ext_expected) {
        ret = 0;
        break;
      }

      if (status == STATUS_TIMEOUT) {
        ret = -ETIMEDOUT;
        break;
      }

      if constexpr (Interruptible) {
        if (status != STATUS_ALERTED) {
          ret = -EINTR;
          break;
        }
      }

      // Non-interruptible: absorb spurious wake and retry.
      nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
      if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
        ret = -ETIMEDOUT;
        break;
      }
    }

    park_state_.store(NOT_PARKED, cpp::MemoryOrder::RELEASE);
    { ::NtWaitForAlertByThreadId(tid_cookie, &DRAIN_TIMEOUT); }

    return ret;
  }

  // ===================================================================
  // Cross-thread API — cold path
  // ===================================================================
  //
  // Static methods operating on a target ThreadLocalWord*. The caller
  // obtains the pointer from a thread registry, shared data structure,
  // or direct reference.
  //
  // [Futex optimization #4: cross-thread writes use atomic stores]
  // [Futex optimization #9: generation bump on every write]

  /// Wake the owner if kernel-parked, without writing to value_.
  ///
  /// Read-only check of park_state_ + conditional NtAlertThreadByThreadId.
  /// The caller has made the wake condition externally visible (e.g., the
  /// kernel wrote cq->Tail); this method provides the NtAlert for the case
  /// where the owner is in kernel sleep and cannot observe the external
  /// change on its own.
  ///
  /// Zero-bounce: the ACQUIRE load of park_state_ transitions the owner's
  /// cache line to Shared (read-share, ~20-40ns snoop) — no write-
  /// invalidation. The owner's next store (clearing KERNEL_PARKED on wake)
  /// is a local Shared→Modified upgrade.
  ///
  /// Use with wait_for_addr(): the owner monitors an external address via
  /// UMWAIT + kernel park. The cross-thread waker calls alert_if_parked()
  /// instead of signal()/signal_or() — no value_ write needed because the
  /// owner re-checks the external address on wake, not value_.
  LIBC_INLINE static void alert_if_parked(ThreadLocalWord *target) {
    if (target->park_state_.load(cpp::MemoryOrder::ACQUIRE) == KERNEL_PARKED) {
      ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(target->owner_tid_)));
    }
  }

  /// Store a new value and wake the owner if kernel-parked.
  ///
  /// If the owner is in UMWAIT/MWAITX (park_state_ == NOT_PARKED), the
  /// atomic store to value_ writes the cache line, which wakes the hardware
  /// monitor — no explicit alert needed. [Futex optimization #5]
  ///
  /// If the owner is in kernel sleep (park_state_ == KERNEL_PARKED),
  /// fires NtAlertThreadByThreadId. [Futex optimization #15 complement]
  LIBC_INLINE static void signal(ThreadLocalWord *target, uint32_t new_value) {
    // Atomic store: on x86 this is a plain MOV (TSO guarantees visibility).
    // On AArch64 this is STLR (store-release).
    // [Futex optimization #4: split-width, atomic on cross-thread path]
    __atomic_store_n(&target->value_, new_value, __ATOMIC_RELEASE);

    // Bump generation. SEQ_CST provides the Dekker fence between the value_
    // store above and the park_state_ load below. On x86, LOCK XADD is
    // already a full barrier regardless of ordering — zero additional cost.
    // On AArch64, ldaxr/stlxr is the same for RELEASE and SEQ_CST — zero
    // additional cost. [Futex optimization #9]
    __atomic_fetch_add(&target->generation_, 1, __ATOMIC_SEQ_CST);

    // Check if owner needs a kernel alert.
    // [Futex optimization #5: if NOT_PARKED, the cache-line write from
    // the store above is the implicit wake signal — no alert needed]
    if (target->park_state_.load(cpp::MemoryOrder::ACQUIRE) == KERNEL_PARKED) {
      ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(target->owner_tid_)));
    }
  }

  /// Set specific flag bits and wake the owner if kernel-parked.
  /// [Futex optimization #13: pending-flags bitmask]
  ///
  /// Uses atomic OR: multiple cross-thread writers can set different bits
  /// concurrently without losing updates. The owner clears handled bits
  /// via clear_flags().
  LIBC_INLINE static void signal_or(ThreadLocalWord *target, uint32_t bits) {
    __atomic_fetch_or(&target->value_, bits, __ATOMIC_RELEASE);
    // SEQ_CST: Dekker fence between value_ write and park_state_ read.
    __atomic_fetch_add(&target->generation_, 1, __ATOMIC_SEQ_CST);

    if (target->park_state_.load(cpp::MemoryOrder::ACQUIRE) == KERNEL_PARKED) {
      ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(target->owner_tid_)));
    }
  }

  /// Store a new value WITHOUT waking the owner. Use when the owner will
  /// poll eventually and doesn't need immediate notification. Avoids the
  /// park_state_ load and potential kernel alert syscall.
  ///
  /// The cache-line write still wakes a UMWAIT/MWAITX monitor — the
  /// "no wake" only skips the explicit NtAlert for kernel sleepers.
  LIBC_INLINE static void store(ThreadLocalWord *target, uint32_t new_value) {
    __atomic_store_n(&target->value_, new_value, __ATOMIC_RELEASE);
    // SEQ_CST ensures the generation bump is ordered after the value_ store
    // in the modification order. The owner's changed_since() reads generation_
    // with RELAXED — without SEQ_CST here, the owner could see a bumped
    // generation but stale value_ (reordered on a weakly-ordered architecture).
    __atomic_fetch_add(&target->generation_, 1, __ATOMIC_SEQ_CST);
  }

  /// Set flag bits WITHOUT waking the owner. Deferred-signal variant.
  LIBC_INLINE static void store_or(ThreadLocalWord *target, uint32_t bits) {
    __atomic_fetch_or(&target->value_, bits, __ATOMIC_RELEASE);
    __atomic_fetch_add(&target->generation_, 1, __ATOMIC_SEQ_CST);
  }

  /// Clear specific flag bits and wake the owner if kernel-parked.
  /// Cross-thread variant of clear_flags(). Used by resume paths to
  /// atomically clear a condition bit and unblock the owner from
  /// wait_for_change() or NtWaitForAlertByThreadId.
  ///
  /// Ordering: RELEASE fetch_and ensures the bit-clear is visible before
  /// any subsequent operations on the calling thread. SEQ_CST generation
  /// bump provides the Dekker fence between the value_ write and the
  /// park_state_ load, matching signal()/signal_or() semantics.
  LIBC_INLINE static void signal_clear_bits(ThreadLocalWord *target,
                                             uint32_t mask) {
    __atomic_fetch_and(&target->value_, ~mask, __ATOMIC_RELEASE);
    __atomic_fetch_add(&target->generation_, 1, __ATOMIC_SEQ_CST);

    if (target->park_state_.load(cpp::MemoryOrder::ACQUIRE) == KERNEL_PARKED) {
      ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(target->owner_tid_)));
    }
  }

private:
  // ---------------------------------------------------------------------------
  // Timeout conversion (shared with Futex)
  // ---------------------------------------------------------------------------

  LIBC_INLINE LARGE_INTEGER *
  timeout_to_nt(cpp::optional<Timeout> timeout, LARGE_INTEGER &storage) {
    if (!timeout)
      return nullptr;

    constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;
    const timespec &ts = timeout->get_timespec();
    long long target_hns =
        static_cast<long long>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100;

    if (timeout->is_realtime()) {
      storage.QuadPart = target_hns + EPOCH_DIFF_HNS;
      return &storage;
    }

    ULONGLONG now_hns;
    ::RtlQueryUnbiasedInterruptTime(&now_hns);
    long long relative_hns = target_hns - static_cast<long long>(now_hns);
    if (relative_hns <= 0) {
      storage.QuadPart = -1;
      return &storage;
    }
    storage.QuadPart = -relative_hns;
    return &storage;
  }

  // ---------------------------------------------------------------------------
  // Hardware address monitor sleep
  // ---------------------------------------------------------------------------
  //
  // [Futex opt #2: UMWAIT/MWAITX hardware address monitor]
  // [Futex opt #5: cache-line write = implicit wake]
  // [Futex opt #6: KUSER_SHARED_DATA spin budget]
  // [Futex opt #7: TSC-bounded sleep]
  //
  // Monitors the cache line containing value_ with UMWAIT (Intel 12th gen+)
  // or MWAITX (AMD Zen+). Returns true if value_ != expected after the
  // monitor phase, false if the TSC budget expired without a change.
  //
  // Budget: threshold × 2048 TSC ticks. This is 2× the Futex's spin budget
  // (threshold × 1024) because:
  //   - Single owner: no Treiber push contention to fall through quickly.
  //   - Higher value of catching the change: avoiding the kernel sleep path
  //     saves ~1-2µs of syscall overhead + context switch scheduling latency.
  //   - No SMT starvation risk: one thread sleeping in C0.2 doesn't steal
  //     resources from siblings (unlike PAUSE spin).
  //
  // At threshold=16 (common AMD default), 4.5GHz: ~7.3µs budget.
  // At threshold=4096 (common Intel default), 4.5GHz: ~1.9ms budget.
  //
  // The monitor sequence is:
  //   1. UMONITOR/MONITORX: arm cache-line monitor on &value_
  //   2. Load value_: if changed, return true (monitor setup was harmless)
  //   3. UMWAIT/MWAITX: sleep until cache-line write or TSC deadline
  //   4. Loop back to 1 (the monitor is one-shot — must re-arm)
  //
  // The load in step 2 is __atomic_load_n(RELAXED) to prevent the compiler
  // from hoisting it above the asm block (which would defeat the monitor).
  // On x86, RELAXED load = plain MOV — zero overhead.
  LIBC_INLINE bool monitor_sleep(uint32_t expected) {
    uint16_t threshold = spin_wait::kuser_spin_threshold();
    if (threshold < 2)
      return __atomic_load_n(&value_, __ATOMIC_RELAXED) != expected;

    spin_wait::SpinBackend backend = spin_wait::get_backend();

#if defined(__x86_64__) || defined(_M_X64)
    if (backend == spin_wait::SpinBackend::UMWAIT) {
      // TSC budget: threshold × 2048. See budget rationale above.
      uint64_t deadline = __builtin_ia32_rdtsc() +
                          static_cast<uint64_t>(threshold) * 2048;
      for (;;) {
        // Arm the address monitor on value_'s cache line.
        __asm__ volatile("umonitor %0"
                         : : "r"(&value_) : "memory");

        // Check value. Must reload — the asm "memory" clobber forces it.
        if (__atomic_load_n(&value_, __ATOMIC_RELAXED) != expected)
          return true;

        uint64_t now = __builtin_ia32_rdtsc();
        if (now >= deadline)
          return false;

        // Enter C0.2 sleep (eax=1). Near-zero power, near-zero SMT
        // resource usage. Wakes on:
        //   (a) Store to value_'s cache line (the implicit wake)
        //   (b) TSC reaching deadline
        //   (c) Any interrupt (NMI, SMI, APC)
        uint32_t lo = static_cast<uint32_t>(deadline);
        uint32_t hi = static_cast<uint32_t>(deadline >> 32);
        uint8_t cf;
        __asm__ volatile("umwait %[ctl]"
                         : "=@ccc"(cf)
                         : [ctl] "r"(1u), "d"(hi), "a"(lo)
                         : "memory");
      }
    }

    if (backend == spin_wait::SpinBackend::MWAITX) {
      uint64_t deadline = __builtin_ia32_rdtsc() +
                          static_cast<uint64_t>(threshold) * 2048;
      for (;;) {
        // monitorx: arm address monitor. addr in rax, ecx=0, edx=0.
        __asm__ volatile("monitorx"
                         : : "a"(&value_), "c"(0), "d"(0) : "memory");

        if (__atomic_load_n(&value_, __ATOMIC_RELAXED) != expected)
          return true;

        uint64_t now = __builtin_ia32_rdtsc();
        if (now >= deadline)
          return false;

        uint64_t remaining = deadline - now;
        uint32_t ticks = remaining > 0xFFFFFFFFu
                             ? 0xFFFFFFFFu
                             : static_cast<uint32_t>(remaining);
        // mwaitx: ecx=2 (use ebx as TSC timer), ebx=ticks.
        __asm__ volatile("mwaitx"
                         : : "a"(0), "c"(2), "b"(ticks)
                         : "memory");
      }
    }
#endif // x86_64

    // Fallback: bounded PAUSE/YIELD spin.
    //
    // Unlike the Futex's spin_on_link_state (which skips PAUSE fallback
    // because multiple threads spinning would starve the lock holder),
    // PAUSE is acceptable here: single owner means at most ONE thread
    // is spinning, so no SMT starvation risk. Capped at 64 iterations
    // to limit worst-case latency to ~900ns on a 4.5GHz Skylake
    // (PAUSE = ~14 cycles post-Skylake).
    uint16_t count = threshold < 64 ? threshold : 64;
    for (uint16_t i = 0; i < count; ++i) {
      if (__atomic_load_n(&value_, __ATOMIC_RELAXED) != expected)
        return true;
      spin_wait::relax_processor();
    }
    return __atomic_load_n(&value_, __ATOMIC_RELAXED) != expected;
  }

  // ---------------------------------------------------------------------------
  // Hardware address monitor on an arbitrary address
  // ---------------------------------------------------------------------------
  //
  // Same hardware monitor sequence as monitor_sleep(), but armed on an
  // external address instead of &value_. Used by wait_for_addr() Phase 0
  // to monitor the primary data source (e.g., kernel-mapped IoRing CQ tail)
  // before falling to the value_-based Phase 1.
  //
  // Budget: threshold × 2048 TSC ticks (same as monitor_sleep).
  // This is the sole hardware monitor phase in wait_for_addr() — the
  // full TLW budget is allocated here. No secondary value_-based phase
  // follows, so there's no budget to split.
  LIBC_INLINE bool monitor_sleep_on(const volatile uint32_t *addr,
                                     uint32_t expected) {
    uint16_t threshold = spin_wait::kuser_spin_threshold();
    if (threshold < 2)
      return __atomic_load_n(addr, __ATOMIC_RELAXED) != expected;

    spin_wait::SpinBackend backend = spin_wait::get_backend();

#if defined(__x86_64__) || defined(_M_X64)
    if (backend == spin_wait::SpinBackend::UMWAIT) {
      uint64_t deadline = __builtin_ia32_rdtsc() +
                          static_cast<uint64_t>(threshold) * 2048;
      for (;;) {
        __asm__ volatile("umonitor %0" : : "r"(addr) : "memory");

        if (__atomic_load_n(addr, __ATOMIC_RELAXED) != expected)
          return true;

        uint64_t now = __builtin_ia32_rdtsc();
        if (now >= deadline)
          return false;

        uint32_t lo = static_cast<uint32_t>(deadline);
        uint32_t hi = static_cast<uint32_t>(deadline >> 32);
        uint8_t cf;
        __asm__ volatile("umwait %[ctl]"
                         : "=@ccc"(cf)
                         : [ctl] "r"(1u), "d"(hi), "a"(lo)
                         : "memory");
      }
    }

    if (backend == spin_wait::SpinBackend::MWAITX) {
      uint64_t deadline = __builtin_ia32_rdtsc() +
                          static_cast<uint64_t>(threshold) * 2048;
      for (;;) {
        __asm__ volatile("monitorx" : : "a"(addr), "c"(0), "d"(0) : "memory");

        if (__atomic_load_n(addr, __ATOMIC_RELAXED) != expected)
          return true;

        uint64_t now = __builtin_ia32_rdtsc();
        if (now >= deadline)
          return false;

        uint64_t remaining = deadline - now;
        uint32_t ticks = remaining > 0xFFFFFFFFu
                             ? 0xFFFFFFFFu
                             : static_cast<uint32_t>(remaining);
        __asm__ volatile("mwaitx" : : "a"(0), "c"(2), "b"(ticks) : "memory");
      }
    }
#endif // x86_64

    // Fallback: bounded PAUSE/YIELD spin on the external address.
    uint16_t count = threshold < 64 ? threshold : 64;
    for (uint16_t i = 0; i < count; ++i) {
      if (__atomic_load_n(addr, __ATOMIC_RELAXED) != expected)
        return true;
      spin_wait::relax_processor();
    }
    return __atomic_load_n(addr, __ATOMIC_RELAXED) != expected;
  }
};

// --- Layout validation ---
// [Futex optimization #8: cache-line alignment]
static_assert(sizeof(ThreadLocalWord) == 64,
              "ThreadLocalWord must be exactly one cache line");
static_assert(alignof(ThreadLocalWord) == 64,
              "ThreadLocalWord must be cache-line aligned");

// Verify field offsets for the hot-path invariant: value_ and generation_
// are in the first 8 bytes, so a single cache-line fetch loads both.
static_assert(__builtin_offsetof(ThreadLocalWord, value_) == 0,
              "value_ must be at offset 0");
static_assert(__builtin_offsetof(ThreadLocalWord, generation_) == 4,
              "generation_ must be at offset 4");
static_assert(__builtin_offsetof(ThreadLocalWord, owner_tid_) == 8,
              "owner_tid_ must be at offset 8");
static_assert(__builtin_offsetof(ThreadLocalWord, park_state_) == 12,
              "park_state_ must be at offset 12");

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LOCAL_WORD_H
