//===-- Process control (Layer 4) --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SIGSTOP/SIGTSTP/SIGCONT/SIGTTIN/SIGTTOU process control.
//
// Owns the cooperative stop protocol, coordinator watchdog,
// kernel-level SIGSTOP, resume, and parent state-change notification.
// Independent state machine from the Layer 3 dispatch engine.
//
// Key improvement over signal_delivery.cpp:
//   - StopState tracks coordinator TID + SystemTime. Any waiting thread
//     can steal the coordinator role after timeout via CAS, preventing
//     the entire process from hanging if the coordinator dies or stalls.
//
// Three-phase stop protocol preserved from existing code:
//   Phase 1: Cooperative APC + alert broadcast
//   Phase 2: Second APC round for kernel-wait threads
//   Phase 3a: Suspend-APC-Resume (kernel transition triggers APC)
//   Phase 3b: Hard-suspend (last resort, reversed on SIGCONT)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_PROCESS_CONTROL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_PROCESS_CONTROL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Forward declarations.
struct ThreadSignalState;

namespace process_control {

// ---------------------------------------------------------------------------
// Stop constants — used by process_control.cpp and signal_state.cpp
// ---------------------------------------------------------------------------
//
// Stop coordination state now lives in the PCB under g_pcb.signal_stop.
// The old StopState struct and get_stop_state() singleton have been removed.

// Stop phases (per-thread).
enum StopPhase : uint8_t {
  STOP_RUNNING = 0,
  STOP_REQUESTED = 1,
  STOP_STOPPED = 2,
  STOP_HARD_SUSPENDED = 3,
};

// Process-level stop phases (stored in g_pcb.signal_stop.phase as uint32_t).
inline constexpr uint32_t PHASE_RUNNING = 0;
inline constexpr uint32_t PHASE_STOPPING = 1;
inline constexpr uint32_t PHASE_STOPPED = 2;

// Coordinator liveness timeout: 500ms in 100ns ticks.
inline constexpr uint32_t STOP_COORDINATOR_TIMEOUT_TICKS = 5000000;

// Spin counts for cooperative wait phases.
inline constexpr int PHASE1_SPINS = 2048;
inline constexpr int PHASE2_SPINS = 4096;
inline constexpr int SETTLE_SPINS = 2048;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Execute the default action for a signal. Dispatched by signal number:
//   - Terminate/Core: NtTerminateProcess(128 + signum)
//   - SIGSTOP: kernel-level process suspension (NtChangeProcessState)
//   - SIGTSTP/SIGTTIN/SIGTTOU: cooperative three-phase stop
//   - SIGCONT: resume all stopped threads
//   - Ignore: SIGCHLD/SIGURG/SIGWINCH no-op
void execute_default_action(int signum);

// Resume all stopped threads. Called unconditionally when SIGCONT is
// generated — even if blocked, ignored, or caught (POSIX requirement).
// Reverses both cooperative parks and hard-suspends.
void resume_stopped_threads();

// Park the calling thread if a stop has been requested.
// Called at dispatch boundaries and from the stop checkpoint APC.
// Counts acked threads, sleeps via NtWaitForAlertByThreadId with
// timeout, and checks for coordinator death.
void check_stop_request(ThreadSignalState *state);

// Notify parent of a state change (stop/continue) via the inherited
// ChildStateBlock shared memory region.
void notify_parent_state_change(int state_code);

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_PROCESS_CONTROL_H
