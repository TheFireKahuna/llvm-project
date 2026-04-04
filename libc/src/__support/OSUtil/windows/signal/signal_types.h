//===-- Windows signal types and constants -------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-thread signal state, constants, and lightweight inline helpers. This is
// the base header for the signal subsystem — included by all external consumers
// (pthread, File, unistd, time, etc.) via signal.h.
//
// Heavyweight types (SigqueueEntry, cross-process ABI types) are in
// signal_internal.h. Process-wide signal state lives in the PCB as canonical
// typed state blocks such as g_pcb.signal_handler and g_pcb.signal_dispatch.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_TYPES_H
#define LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_TYPES_H

#include "hdr/signal_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/stack_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/dispatch/restart_state.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/signal_constants.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// Forward declaration — full definition in thread_local_word.h.
struct ThreadLocalWord;

namespace signal_state {

// Forward declarations — full definitions in signal_internal.h / syscall_frame.h.
struct SyscallFrame;

// ---------------------------------------------------------------------------
// Static assertions and sigset_t conversion
// ---------------------------------------------------------------------------

static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(__NSIGSET_WORDS == 2,
              "signal code requires two-word sigset_t for 64 signals");
static_assert(sizeof(unsigned long) == 4,
              "Windows LLP64: unsigned long must be 32 bits");

// ---------------------------------------------------------------------------
// Small inline helpers
// ---------------------------------------------------------------------------

// Alert a thread by TID. Wraps the cast from DWORD to HANDLE that
// NtAlertThreadByThreadId requires.
LIBC_INLINE void alert_thread(DWORD tid) {
  NtAlertThreadByThreadId(
      reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tid)));
}

LIBC_INLINE bool is_valid_signal(int signum) {
  return signum > 0 && signum < NSIG;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr size_t SIGNAL_STACK_HEADROOM = MINSIGSTKSZ;

// Cancel state bit positions. Stored in ThreadLifecycle::cancel_state.
// The pending notification (old bit 2) now lives in notify_word
// (notify::CANCEL) — see thread_lifecycle.h.
inline constexpr uint8_t CANCEL_STATE_BIT   = 0x01; // 0 = ENABLE, 1 = DISABLE
inline constexpr uint8_t CANCEL_TYPE_BIT    = 0x02; // 0 = DEFERRED, 1 = ASYNC

// STOP_SIGNALS_MASK, SIGCONT_MASK, SIGNAL_BITS_MASK, sigset_to_bits,
// bits_to_sigset are in signal_constants.h (leaf header shared with
// pending_storage.h to break include cycles).

// ---------------------------------------------------------------------------
// ThreadSignalState — per-thread, TLS-stored
// ---------------------------------------------------------------------------

struct ThreadSignalState {
  // -------------------------------------------------------------------------
  // Exit-gate bit layout
  // -------------------------------------------------------------------------
  //
  // `exit_gate` coordinates cross-thread writers (pthread_kill senders) with
  // thread-exit drain (deregister_thread_state). Without it, a sender who
  // pends after cleanup's drain completes leaves the signal orphaned in
  // state->pending — see signal drop audit 2026-04-20.
  //
  // Bit 0 (DRAINING): set by the exiting thread's cleanup. New senders who
  //   observe this bit on their acquire skip state->pending entirely and
  //   redirect the signal to process_pending.
  //
  // Bits 1..31 (IN_FLIGHT count): incremented by a sender before touching
  //   state->pending, decremented after. Cleanup spins until this count
  //   reaches zero before draining, guaranteeing the drain is a true single
  //   consumer with no concurrent producers.
  //
  // This makes the drain trivially correct for both standard signals
  // (idempotent bitmap OR) and RT signals (MPSC queue where try_pop is
  // single-consumer by contract): no second drain pass, no SEQ_CST fence
  // on the sender's post-pend path, and no race window where a late push
  // ends up stranded in state->pending past cleanup.
  static constexpr uint32_t EXIT_GATE_DRAINING = 1u << 0;
  static constexpr uint32_t EXIT_GATE_IN_FLIGHT_STEP = 1u << 1;

  // --- 8-byte aligned fields (cache line 1: hot signal path) ---

  void *last_fault_pc;
  void *alt_stack_sp;
  size_t alt_stack_size;

  sigset_t blocked_signals;

  // Per-thread pending signal storage. Standard signals use the atomic
  // bitmap; RT signals use the Vyukov MPSC inbox queue. Replaces the old
  // pending_signals + rt_queue_head pair.
  PendingSet pending;

  // Cross-thread writer / exit drain coordination. Protocol documented on
  // EXIT_GATE_DRAINING above.
  cpp::Atomic<uint32_t> exit_gate;

  // Signals this thread is waiting for in sigsuspend/sigtimedwait.
  // Set by the wait path, checked by pend operations to alert the thread.
  cpp::Atomic<uint64_t> waiting_signals;

  // --- 4-byte aligned fields ---

  DWORD last_fault_code;

  // --- Small fields (packed, no inter-field padding) ---

  unsigned int alt_stack_flags; // SS_ONSTACK | SS_DISABLE | SS_AUTODISARM
  bool owned_state;

  // Cooperative stop parking flag. Set by the owner thread (RELAXED store)
  // before acking via the ack_gen CAS. The ack CAS's ACQ_REL fence carries
  // this store to the coordinator's ACQUIRE load of ack_gen, establishing
  // happens-before for the coordinator's RELAXED read.
  //
  // Atomic<bool> because the coordinator reads cross-thread (skip
  // optimization in Phase 2/3). RELAXED on both sides: same codegen as
  // plain bool on x86 (MOV), eliminates formal data-race UB.
  //
  // The STOP notification itself uses notify::STOP bit in the owning
  // ThreadLifecycle's notify_word (set via signal_or, checked via test_flags).
  cpp::Atomic<bool> stop_parked;

  // Hard-suspended by the coordinator during Phase 3b. Set (RELAXED)
  // while the target is kernel-suspended. Read/cleared by
  // resume_stopped_threads on the SIGCONT handler thread. Atomic<bool>
  // to eliminate formal data-race UB between the stop coordinator and
  // the resume thread. RELAXED on both sides: the process-wide
  // phase store (RELEASE) / load (ACQUIRE) provides the happens-before.
  cpp::Atomic<bool> hard_suspended;

  cpp::Atomic<int8_t> sched_policy;       // SCHED_OTHER / SCHED_FIFO / SCHED_RR

  // Dispatch reentrancy guard. Owner-only — true while dispatch_pending()
  // is actively draining signals on this thread. Prevents nested dispatch
  // when a signal handler calls a function that hits a dispatch boundary.
  //
  // Cross-thread signal notification has moved to the ThreadLocalWord
  // notify_word on the owning ThreadLifecycle (notify::SIGNAL bit). The
  // old three-state atomic (IDLE/TRIGGERED/DRAINING) is replaced by:
  //   - notify::SIGNAL bit for cross-thread "you have signals" notification
  //   - this bool for owner-thread reentrancy prevention
  bool dispatching;

  // Back-pointer to the owning ThreadLifecycle's notify_word. Set during
  // register_thread_state(). Used by trigger() to set notify::SIGNAL via
  // ThreadLocalWord::signal_or() without traversing the lifecycle pointer.
  // nullptr for threads without signal state.
  ThreadLocalWord *notify_word;

  // SA_RESTART stack. Push/pop per nesting level. Replaces the
  // old one-shot last_signal_had_restart scalar.
  RestartState restart;

  // --- Thread-local fields (only the owning thread reads/writes) ---
  // Not Atomic: no cross-thread access. Grouped here to make the
  // thread-local contract visible and prevent accidental "fixes" to Atomic.

  // Interrupted thread context, valid only during APC/VEH dispatch frame.
  // Borrowed pointer — NOT owned. Set by the APC callback (via
  // APC_CALLBACK_DATA_CONTEXT) or VEH handler (from EXCEPTION_POINTERS),
  // used by dispatch_engine to construct ucontext_t for SA_SIGINFO handlers,
  // then cleared after dispatch returns. nullptr for deferred delivery.
  CONTEXT *interrupted_context;

  // Deferred re-fault handler reset. VEH stores the signal number here
  // instead of writing sa_handler = SIG_DFL lock-free. dispatch_pending
  // resets the handler under the global lock. 0 = no deferred reset.
  int8_t refault_reset_signum;

  // Set while inside invoke_handler. VEH checks this to prevent
  // recursive signal delivery when a handler faults.
  bool in_signal_handler;

  // Set by invoke_handler after a signal handler returns. Read and
  // cleared by sigsuspend to detect that a handler ran (even when the
  // signal was delivered directly, not via the pending set).
  bool handler_ran;

  // SS_AUTODISARM save state. When a handler is entered on an alt-stack
  // with SS_AUTODISARM, the alt-stack settings are saved here and the
  // alt-stack is disarmed (SS_DISABLE). The dispatch engine restores
  // these on handler return (HandlerScope dtor) or longjmp recovery.
  // nullptr when no autodisarm is active.
  void *autodisarm_saved_sp;
  size_t autodisarm_saved_size;
  unsigned int autodisarm_saved_flags;
};

// Zero-initialization safety assertions.
static_assert(__is_trivially_constructible(cpp::Atomic<uint64_t>),
              "Atomic<uint64_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<uint32_t>),
              "Atomic<uint32_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<bool>),
              "Atomic<bool> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<int8_t>),
              "Atomic<int8_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<int>),
              "Atomic<int> must be trivially constructible");

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_TYPES_H
