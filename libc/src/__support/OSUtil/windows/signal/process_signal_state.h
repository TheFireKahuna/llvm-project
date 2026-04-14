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
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {

struct ThreadLifecycle;

namespace signal_state {

struct SignalHandlerState {
  struct sigaction handlers[NSIG];
  RawMutex lock;
  cpp::Atomic<uint64_t> ignored;
  cpp::Atomic<uint64_t> custom;
};

struct SignalDispatchState {
  PendingSet process_pending;
  cpp::Atomic<ThreadLifecycle *> preferred_thread;
  cpp::Atomic<uint64_t> preferred_blocked;
};

struct SignalSigchldState {
  cpp::Atomic<uint64_t> packed;
};

struct SignalTransportState {
  cpp::Atomic<uint32_t> veh_registered;
  cpp::Atomic<uint32_t> console_registered;
  cpp::Atomic<uint32_t> tty_winsize;
  cpp::Atomic<uint32_t> tty_winsize_initialized;
  cpp::Atomic<uint32_t> tty_hangup_sent;
};

struct SignalChildState {
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
  cpp::Atomic<uint64_t> coordinator;
  cpp::Atomic<uint32_t> target_count;
};

struct SignalAlpcState {
  HANDLE namespace_handle;
  HANDLE port;
  void *boundary;
  uint64_t create_time;
};

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PROCESS_SIGNAL_STATE_H
