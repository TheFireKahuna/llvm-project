//===-- Process-wide signal state for Windows ---------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Canonical lightweight process-resident signal state blocks embedded in the
// PCB. These are passive storage records: signal behavior stays in the signal
// subsystem .cpp files.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PROCESS_SIGNAL_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PROCESS_SIGNAL_STATE_H

#include "hdr/signal_macros.h"
#include "hdr/types/struct_sigaction.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

struct SignalHandlerState {
  struct sigaction handlers[NSIG];
  RawMutex lock;
  cpp::Atomic<uint64_t> ignored;
  cpp::Atomic<uint64_t> custom;
};

struct SignalDispatchState {
  PendingSet process_pending;

  // Packed ThreadHandle of the thread currently chosen as the
  // process-directed signal dispatcher (decoded via
  // ThreadHandle::unpack). Storing a handle rather than a raw
  // ThreadLifecycle pointer is structural defense against
  // TID-recycling-after-retire: every load is re-resolved through
  // registry_resolve, so a recycled task_id slot is rejected. 0 = no
  // preferred thread.
  cpp::Atomic<uint64_t> preferred_thread;
  cpp::Atomic<uint64_t> preferred_blocked;

  // Cross-process sender identity (pid/uid/sival) is no longer kept in
  // the dispatch state. ALPC transport now publishes it into the wait-
  // free Crystalline-backed payload subsystem at signal/payload/, and
  // build_standard_siginfo reads it back via populate_signal_payload.
  // The previous process_sender[] side-array had a torn-tuple race with
  // concurrent same-signum senders — fixed structurally by giving each
  // event its own immutable record.
};


struct SignalTransportState {
  cpp::Atomic<uint32_t> veh_registered;
  cpp::Atomic<uint32_t> console_registered;
  cpp::Atomic<uint32_t> tty_winsize;
  cpp::Atomic<uint32_t> tty_winsize_initialized;
  cpp::Atomic<uint32_t> tty_hangup_sent;
};

struct SignalChildState {
  // --- Single-threaded-access fields ---
  // Written during signal_subsystem_init() (Phase 7) or fork child setup.
  // No concurrent access — protected by init phase ordering and the fact
  // that fork children start single-threaded. Do NOT make these Atomic.
  HANDLE process_state_change;
  HANDLE inherited_event;
  void *inherited_region;
};

struct SignalStopState {
  cpp::Atomic<uint32_t> phase;
  // Packed generation (high 32) + ack count (low 32). A single 64-bit
  // CAS atomically validates the generation and increments the count,
  // closing the TOCTOU gap that separate gen/count fields would have.
  cpp::Atomic<uint64_t> ack_gen;
  // Coordinator TID. Non-zero while a cooperative stop is in progress.
  // Used by the reactor death callback to identify which thread died.
  cpp::Atomic<uint32_t> coordinator_tid;
  cpp::Atomic<uint32_t> target_count;

  // Coordinator thread handle — borrowed from the thread registry for
  // the duration of the stop protocol. The reactor watches this handle;
  // when it signals (thread exits), the death callback fires.
  // Cleared by resume_stopped_threads and fork_reinit.
  HANDLE coordinator_handle;

  // Reactor watch token for the coordinator death watch. Valid while
  // a cooperative stop is in progress. One-shot WCP: fires once on
  // coordinator exit. Unwatched by resume_stopped_threads.
  internal::reactor::ReactorToken death_watch;

  // CAS flag for death watch registration. Non-coordinator threads
  // race to register the WCP watch on the coordinator's handle —
  // the first to CAS this from false → true does the registration.
  // Reset by resume_stopped_threads and CAS-steal winners.
  cpp::Atomic<bool> death_watch_registered;
};

// SignalAlpcState was folded into internal::AlpcBusState when the signal
// transport was generalized into a multiplexed bus (see ipc/alpc_bus.h).
// The only observable ALPC identity a signal consumer needs today is
// create_time, which it reads via alpc_bus::self_create_time().

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PROCESS_SIGNAL_STATE_H
