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
// Key improvements:
//   - Per-thread stop notification via notify::STOP bit in ThreadLocalWord
//     (zero-atomic fast path: one MOV + TEST to check, signal_or to set)
//   - Park via ThreadLocalWord::wait_for_change() — UMWAIT/MWAITX hardware
//     monitor (~100ns wake) + Dekker kernel sleep (no lost-wake window)
//   - Event-driven coordinator death recovery via reactor WCP watch,
//     registered by a non-coordinator parked thread. No self-registration
//     atomic gap. No polling, no arbitrary timeout.
//
// Three-phase stop protocol:
//   Phase 1: Cooperative APC + alert broadcast
//   Phase 2: Second APC round for kernel-wait threads
//   Phase 3a: Suspend-APC-Resume (kernel transition triggers APC)
//   Phase 3b: Hard-suspend (last resort, reversed on SIGCONT)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_PROCESS_CONTROL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_PROCESS_CONTROL_H

#include "src/__support/macros/config.h"

#include "hdr/stdint_proxy.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Forward declarations.
struct ThreadSignalState;

namespace process_control {

// ---------------------------------------------------------------------------
// Stop constants — used by process_control.cpp and signal_state.cpp
// ---------------------------------------------------------------------------
//
// Stop coordination state lives in the PCB under g_pcb.signal_stop.
//
// Per-thread stop notification uses the notify::STOP bit in the owning
// ThreadLifecycle's notify_word. The coordinator sets the bit via
// ThreadLocalWord::signal_or(); each thread checks it via test_flags()
// (one MOV + TEST, zero atomics). Parking state is tracked by
// ThreadSignalState::stop_parked (Atomic<bool>, owner-written RELAXED,
// coordinator-read RELAXED) and hard-suspension by
// ThreadSignalState::hard_suspended (Atomic<bool>, coordinator-written
// while target is kernel-suspended, resume-thread-read RELAXED).

// Process-level stop phases (stored in g_pcb.signal_stop.phase as uint32_t).
inline constexpr uint32_t PHASE_RUNNING = 0;
inline constexpr uint32_t PHASE_STOPPING = 1;
inline constexpr uint32_t PHASE_STOPPED = 2;

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
// Counts acked threads, parks via ThreadLocalWord::wait_for_change()
// (UMWAIT/MWAITX hardware monitor → Dekker kernel sleep). Non-coordinator
// threads register the coordinator's death watch (WCP) before parking.
// Coordinator death is event-driven — no timeout, no polling.
void check_stop_request(ThreadSignalState *state);

// Called from lifecycle_cleanup when a thread exits. If this thread is
// the stop coordinator, broadcasts COORD_DEAD to all parked threads.
// Cheap no-op (~1 cycle RELAXED load) for non-coordinator threads.
void on_coordinator_exit(uint32_t tid);

// Notify parent of a state change (stop/continue) via the inherited
// ChildStateBlock shared memory region.
void notify_parent_state_change(int state_code);

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_PROCESS_CONTROL_H
