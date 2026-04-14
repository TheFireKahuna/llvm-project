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

// Cancellation word bit positions. Packed into ThreadSignalState::cancel_word.
inline constexpr uint8_t CANCEL_STATE_BIT   = 0x01; // 0 = ENABLE, 1 = DISABLE
inline constexpr uint8_t CANCEL_TYPE_BIT    = 0x02; // 0 = DEFERRED, 1 = ASYNC
inline constexpr uint8_t CANCEL_PENDING_BIT = 0x04; // 1 = cancellation requested
// cancel_check fast test: enabled AND pending in one compare.
inline constexpr uint8_t CANCEL_CHECK_MASK  = CANCEL_STATE_BIT | CANCEL_PENDING_BIT;

// STOP_SIGNALS_MASK, SIGCONT_MASK, SIGNAL_BITS_MASK, sigset_to_bits,
// bits_to_sigset are in signal_constants.h (leaf header shared with
// pending_storage.h to break include cycles).

// ---------------------------------------------------------------------------
// ThreadSignalState — per-thread, TLS-stored
// ---------------------------------------------------------------------------

struct ThreadSignalState {
  // --- 8-byte aligned fields (cache line 1: hot signal path) ---

  void *last_fault_pc;
  void *alt_stack_sp;
  size_t alt_stack_size;

  sigset_t blocked_signals;

  // Per-thread pending signal storage. Standard signals use the atomic
  // bitmap; RT signals use the Vyukov MPSC inbox queue. Replaces the old
  // pending_signals + rt_queue_head pair.
  PendingSet pending;

  // Signals this thread is waiting for in sigsuspend/sigtimedwait.
  // Set by the wait path, checked by pend operations to alert the thread.
  cpp::Atomic<uint64_t> waiting_signals;

  // --- 4-byte aligned fields ---

  DWORD last_fault_code;
  int last_fault_si_code; // POSIX si_code for last VEH-sourced fault.

  // --- Small fields (packed, no inter-field padding) ---

  unsigned short alt_stack_flags; // SS_ONSTACK | SS_DISABLE
  bool owned_state;

  // Per-thread cooperative stop state. Values from process_control::StopPhase.
  cpp::Atomic<unsigned char> stop_state;
  cpp::Atomic<int8_t> sched_policy;       // SCHED_OTHER / SCHED_FIFO / SCHED_RR

  // Dispatch state machine. Values from DispatchState enum (dispatch_engine.h).
  // Transitions:
  //   IDLE --(transport pends)--> TRIGGERED --(dispatch boundary)--> DRAINING
  //     ^                                                              |
  //     +---------(drain complete, re-check finds nothing)-------------+
  // DRAINING→IDLE re-checks pending bits + CAS to close the window where
  // a signal is pended between the last drain iteration and the state reset.
  cpp::Atomic<uint8_t> dispatch_state;

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
};

// Zero-initialization safety assertions.
static_assert(__is_trivially_constructible(cpp::Atomic<uint64_t>),
              "Atomic<uint64_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<bool>),
              "Atomic<bool> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<unsigned char>),
              "Atomic<unsigned char> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<int>),
              "Atomic<int> must be trivially constructible");

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_TYPES_H
