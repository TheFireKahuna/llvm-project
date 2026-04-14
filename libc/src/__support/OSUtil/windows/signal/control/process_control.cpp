//===-- Process control (Layer 4) -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SIGSTOP/SIGTSTP/SIGCONT process stop/resume protocol.
//
// Coordinator death recovery: the coordinator is tracked by TID + SystemTime
// stamp packed into a single atomic uint64. Waiting threads detect a dead or
// stalled coordinator after STOP_COORDINATOR_TIMEOUT_TICKS (500ms) and steal
// the role via CAS.
//
// Three-phase stop protocol preserved from signal_delivery.cpp:
//   Phase 1: Cooperative APC + alert
//   Phase 2: Second APC round for kernel-wait threads
//   Phase 3a: Suspend-APC-Resume (kernel transition fires APC)
//   Phase 3b: Hard-suspend (last resort)
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/control/process_control.h"

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/apc.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/restart_state.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace process_control {

// ---------------------------------------------------------------------------
// SystemTime low-32 helper (VEH-safe, no API call on x86_64/aarch64)
// ---------------------------------------------------------------------------

// Read the low 32 bits of the shared user data SystemTime.
// This is a user-mode-readable kernel page at a fixed address.
// Resolution: 100ns ticks. Low 32 bits wrap every ~429 seconds.
static LIBC_INLINE uint32_t system_time_lo() {
  // SharedUserData is at 0x7FFE0000 on all Windows versions.
  // SystemTime is at offset 0x14 (KSYSTEM_TIME structure).
  // The Low field is the first ULONG at that offset.
  auto *sud = reinterpret_cast<const volatile uint32_t *>(0x7FFE0014);
  return *sud;
}

static LIBC_INLINE uint64_t pack_coordinator(uint32_t tid, uint32_t start) {
  return (static_cast<uint64_t>(tid) << 32) | start;
}

static LIBC_INLINE uint32_t unpack_coordinator_tid(uint64_t packed) {
  return static_cast<uint32_t>(packed >> 32);
}

static LIBC_INLINE uint32_t unpack_coordinator_start(uint64_t packed) {
  return static_cast<uint32_t>(packed);
}

// ---------------------------------------------------------------------------
// Packed ack_gen helpers: high 32 = generation, low 32 = ack count.
// A single 64-bit CAS validates generation and increments count atomically.
// ---------------------------------------------------------------------------

static LIBC_INLINE uint64_t pack_ack_gen(uint32_t gen, uint32_t count) {
  return (static_cast<uint64_t>(gen) << 32) | count;
}

static LIBC_INLINE uint32_t ack_gen_generation(uint64_t v) {
  return static_cast<uint32_t>(v >> 32);
}

static LIBC_INLINE uint32_t ack_gen_count(uint64_t v) {
  return static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------------------
// Coordinator liveness check
// ---------------------------------------------------------------------------
//
// Check if the current coordinator is dead or has stalled beyond the
// timeout. Returns true if the coordinator should be replaced.
//
// Two-step check:
//   1. Timeout: has enough time elapsed since coordinator start?
//      If not, return false (avoid per-wake NtOpenThread overhead).
//   2. Liveness: is the coordinator thread still alive?
//      Open by TID, check for termination. Dead → return true.

static bool is_coordinator_dead_or_stalled() {
  auto &stop = g_pcb.signal_stop;

  // Single atomic load gets both TID and start timestamp — no torn read.
  uint64_t packed = stop.coordinator.load(cpp::MemoryOrder::ACQUIRE);
  uint32_t tid = unpack_coordinator_tid(packed);
  uint32_t start = unpack_coordinator_start(packed);

  if (tid == 0)
    return false;

  uint32_t now = system_time_lo();
  // Unsigned subtraction handles wrap correctly.
  if (now - start < STOP_COORDINATOR_TIMEOUT_TICKS)
    return false;

  // Open the thread to check liveness. Minimal access rights.
  CLIENT_ID cid = {};
  cid.UniqueThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(tid));
  auto oa = windows::internal_oa();
  HANDLE h = nullptr;
  NTSTATUS st = ::NtOpenThread(&h, SYNCHRONIZE, &oa, &cid);
  if (!NT_SUCCESS(st))
    return true; // Thread doesn't exist -- coordinator is dead.

  // Check if the thread has exited. Zero timeout = non-blocking.
  LARGE_INTEGER zero_timeout = {};
  st = ::NtWaitForSingleObject(h, FALSE, &zero_timeout);
  ::NtClose(h);

  // STATUS_WAIT_0 (0) means the thread has exited.
  return st == 0;
}

// ---------------------------------------------------------------------------
// Stop checkpoint APC
// ---------------------------------------------------------------------------
//
// Injected into each thread during the stop protocol. Fires at kernel→user
// transitions (syscall return, context switch, page fault). Calls
// check_stop_request() which parks the thread cooperatively.

static void NTAPI stop_checkpoint_apc(PVOID, PVOID, PVOID) {
  ThreadSignalState *state = get_thread_state_noinit();
  if (state)
    check_stop_request(state);
}

static bool suspend_apc_resume(ThreadLifecycle *node);
static void hard_suspend_one_thread(ThreadLifecycle *node);

// ---------------------------------------------------------------------------
// Phase 3a: Suspend-APC-Resume
// ---------------------------------------------------------------------------
//
// State-change suspend + resume creates a kernel→user transition on the
// target thread. Special APCs fire at such transitions, so queuing a stop
// APC during the suspension window guarantees delivery when the thread
// resumes. The thread parks via check_stop_request with its full register
// state intact (no CONTEXT capture, no stack write, no XSTATE loss).

static bool suspend_apc_resume(ThreadLifecycle *node) {
  if (!registry_suspend(node))
    return false;

  // Re-check: thread may have self-parked between our check and the suspend.
  if (node->signal &&
      node->signal->stop_state.load(cpp::MemoryOrder::ACQUIRE) ==
          STOP_STOPPED) {
    registry_resume(node);
    return true;
  }

  // Queue APC while suspended. On resume, kernel→user transition fires it.
  HANDLE h = registry_borrow_handle(node);
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  if (h)
    status = windows::queue_special_user_apc(h, stop_checkpoint_apc);
  registry_resume(node);
  return NT_SUCCESS(status);
}

// ---------------------------------------------------------------------------
// Phase 3b: Hard-suspend (last resort)
// ---------------------------------------------------------------------------
//
// Only reached if suspend-APC-resume failed. The thread is frozen in place
// by the OS and resumed on SIGCONT via state-change resume.

static void hard_suspend_one_thread(ThreadLifecycle *node) {
  if (!registry_suspend(node))
    return;

  if (!node->signal) {
    // No signal state — just resume, nothing to track.
    registry_resume(node);
    return;
  }

  // CAS stop_state from STOP_REQUESTED → STOP_HARD_SUSPENDED.
  //
  // This closes the race with resume_stopped_threads(): if SIGCONT arrived
  // between our NtSuspend and this CAS, it exchanged stop_state to
  // STOP_RUNNING. The CAS sees STOP_RUNNING (not STOP_REQUESTED), fails,
  // and we resume the thread immediately.
  //
  // Also handles the self-park case: if the thread called check_stop_request
  // and stored STOP_STOPPED before our suspend took effect, the CAS fails
  // and we resume it (it will re-park itself if needed via the APC).
  unsigned char expected = STOP_REQUESTED;
  if (node->signal->stop_state.compare_exchange_strong(
          expected, STOP_HARD_SUSPENDED, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::ACQUIRE)) {
    return; // Success: thread is suspended and properly marked.
  }

  // CAS failed — state changed (SIGCONT set RUNNING, or thread self-parked).
  // Resume the thread unconditionally; it will sort itself out.
  registry_resume(node);
}

// ---------------------------------------------------------------------------
// Phase 3: Stop stragglers surviving both APC rounds
// ---------------------------------------------------------------------------

static void stop_stragglers(signal_state::SignalStopState &stop,
                            DWORD skip_tid) {
  // Phase 3a: suspend-APC-resume for non-parked threads.
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    if (node->signal &&
        node->signal->stop_state.load(cpp::MemoryOrder::ACQUIRE) ==
            STOP_STOPPED)
      return false;
    suspend_apc_resume(node);
    return false;
  }, skip_tid);

  // Settle: APC fires on resume, calls check_stop_request, thread parks.
  bool all_parked = false;
  for (int i = 0; i < SETTLE_SPINS; ++i) {
    if (ack_gen_count(stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE)) >=
        stop.target_count.load(cpp::MemoryOrder::ACQUIRE) - 1) {
      all_parked = true;
      break;
    }
    if ((i & 31) == 31)
      ::NtYieldExecution();
  }

  if (all_parked)
    return;

  // Phase 3b: hard-suspend remaining threads.
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    if (node->signal &&
        node->signal->stop_state.load(cpp::MemoryOrder::ACQUIRE) ==
            STOP_STOPPED)
      return false;
    hard_suspend_one_thread(node);
    return false;
  }, skip_tid);
}

// ---------------------------------------------------------------------------
// Request all threads to stop
// ---------------------------------------------------------------------------

static void request_process_stop(DWORD self_tid) {
  // Pass 1: set stop_state + inject APCs (per-thread logic).
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    if (node->signal)
      node->signal->stop_state.store(STOP_REQUESTED,
                                     cpp::MemoryOrder::RELEASE);
    DWORD tid = node->owner_tid.load(cpp::MemoryOrder::RELAXED);
    if (tid != self_tid) {
      HANDLE h = registry_borrow_handle(node);
      if (h)
        windows::queue_special_user_apc(h, stop_checkpoint_apc);
    }
    return false;
  }, 0); // don't skip self — we need to set its stop_state too
  // Pass 2: batch-alert all threads in one syscall.
  registry_alert_all(0);
}

// ---------------------------------------------------------------------------
// Cooperative three-phase stop (SIGTSTP/SIGTTIN/SIGTTOU)
// ---------------------------------------------------------------------------

static void cooperative_stop(int signum) {
  auto &stop = g_pcb.signal_stop;

  // Only one thread initiates. CAS RUNNING -> STOPPING.
  uint32_t expected = PHASE_RUNNING;
  if (!stop.phase.compare_exchange_strong(expected, PHASE_STOPPING,
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::RELAXED))
    return; // Another thread is already coordinating.

  DWORD self_tid = NtCurrentThreadId();
  uint32_t target = registry_live_count();

  // Set up coordinator tracking. Packed store updates both
  // TID and start atomically — no window for stale timestamp reads.
  stop.coordinator.store(pack_coordinator(self_tid, system_time_lo()),
                         cpp::MemoryOrder::RELEASE);
  // Atomically bump generation and reset count to 0. The packed store
  // ensures no thread can observe the new generation with a stale count
  // or vice versa. Threads in check_stop_request() from a previous cycle
  // will fail their CAS (generation mismatch) and re-ack correctly.
  uint32_t new_gen =
      ack_gen_generation(stop.ack_gen.load(cpp::MemoryOrder::RELAXED)) + 1;
  stop.ack_gen.store(pack_ack_gen(new_gen, 0), cpp::MemoryOrder::RELEASE);
  stop.target_count.store(target, cpp::MemoryOrder::RELEASE);

  bool all_parked = (target <= 1);

  // Phase 1: inject APCs + alerts to all threads except self.
  request_process_stop(self_tid);

  if (!all_parked) {
    for (int i = 0; i < PHASE1_SPINS; ++i) {
      if (ack_gen_count(stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE)) >=
          target - 1) {
        all_parked = true;
        break;
      }
      if ((i & 63) == 63)
        ::NtYieldExecution();
    }
  }

  // Phase 2: second APC round for threads that entered kernel waits.
  if (!all_parked) {
    registry_for_each([&](ThreadLifecycle *node) -> bool {
      if (node->signal &&
          node->signal->stop_state.load(cpp::MemoryOrder::ACQUIRE) ==
              STOP_STOPPED)
        return false;
      HANDLE h = registry_borrow_handle(node);
      if (h)
        windows::queue_special_user_apc(h, stop_checkpoint_apc);
      return false;
    }, self_tid);
    registry_alert_all(self_tid);

    for (int i = 0; i < PHASE2_SPINS; ++i) {
      if (ack_gen_count(stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE)) >=
          target - 1) {
        all_parked = true;
        break;
      }
      if ((i & 31) == 31)
        ::NtYieldExecution();
    }
  }

  // Phase 3: suspend-APC-resume, then hard-suspend for stragglers.
  if (!all_parked)
    stop_stragglers(stop, self_tid);

  // RELEASE ensures all prior stop protocol operations (APC injection,
  // ack counting, hard-suspend) are visible before any thread observes
  // PHASE_STOPPED via an ACQUIRE load.
  stop.phase.store(PHASE_STOPPED, cpp::MemoryOrder::RELEASE);
  notify_parent_state_change(signum);

  // Self-park. check_stop_request sees STOP_REQUESTED (set by
  // request_process_stop on our own node). Returns when SIGCONT.
  ThreadSignalState *self = get_thread_state_noinit();
  if (self)
    check_stop_request(self);
}

// ---------------------------------------------------------------------------
// check_stop_request — per-thread cooperative park
// ---------------------------------------------------------------------------
//
// After timeout, checks coordinator liveness and steals the role if the
// coordinator is dead/stalled. A new coordinator can either finish the
// stop or abort if SIGCONT has arrived.

void check_stop_request(ThreadSignalState *state) {
  if (!state)
    return;
  if (state->stop_state.load(cpp::MemoryOrder::ACQUIRE) == STOP_RUNNING)
    return;

  auto &stop = g_pcb.signal_stop;
  bool counted = false;
  // Snapshot the generation at entry. Used to detect cycle changes.
  uint32_t my_gen =
      ack_gen_generation(stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE));

  // 5-second timeout guards against missed alerts. Negative = relative.
  LARGE_INTEGER timeout;
  timeout.QuadPart = -50000000LL;

  while (true) {
    unsigned char cur = state->stop_state.load(cpp::MemoryOrder::ACQUIRE);
    if (cur == STOP_RUNNING)
      break;

    // Detect generation change: a new stop cycle started while we were
    // parked from a previous one. Reset our counted flag so we re-ack
    // the new cycle via the CAS below.
    uint64_t ag = stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE);
    uint32_t cur_gen = ack_gen_generation(ag);
    if (counted && cur_gen != my_gen) {
      counted = false;
      my_gen = cur_gen;
    }

    // Only transition REQUESTED -> STOPPED. If already STOPPED (re-woke
    // from NtWaitForAlertByThreadId), skip the CAS and re-park directly.
    // If concurrent SIGCONT already set RUNNING, we caught it above.
    if (cur == STOP_REQUESTED) {
      unsigned char expected = STOP_REQUESTED;
      if (!state->stop_state.compare_exchange_strong(
              expected, STOP_STOPPED, cpp::MemoryOrder::RELEASE,
              cpp::MemoryOrder::ACQUIRE)) {
        if (expected == STOP_RUNNING)
          break;
        continue; // Spurious CAS failure — retry.
      }
    }

    if (!counted) {
      // CAS increment: atomically validates generation and bumps count.
      // If generation changed between our read and here (new stop cycle
      // reset count to 0), the CAS fails, ag is refreshed, and we retry
      // — no TOCTOU gap, no double-ack.
      ag = stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE);
      cur_gen = ack_gen_generation(ag);
      uint64_t desired = pack_ack_gen(cur_gen, ack_gen_count(ag) + 1);
      if (stop.ack_gen.compare_exchange_strong(ag, desired,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE)) {
        counted = true;
        my_gen = cur_gen;
      }
      // CAS failure: another thread acked or generation changed.
      // Either way, retry on next iteration — no spin loop needed since
      // we're about to park in NtWaitForAlertByThreadId anyway.
    }

    NtWaitForAlertByThreadId(nullptr, &timeout);

    // After timeout, check if coordinator is still alive. If dead or
    // stalled, attempt to steal the coordinator role and finish the
    // stop protocol.
    if (is_coordinator_dead_or_stalled()) {
      uint64_t old_coordinator =
          stop.coordinator.load(cpp::MemoryOrder::ACQUIRE);
      uint32_t old_tid = unpack_coordinator_tid(old_coordinator);
      uint32_t my_tid = NtCurrentThreadId();
      // Packed CAS atomically swaps both TID and start timestamp — no
      // window where another thread sees our TID with a stale timestamp.
      uint32_t my_start = system_time_lo();
      if (old_tid != 0 &&
          stop.coordinator.compare_exchange_strong(
              old_coordinator, pack_coordinator(my_tid, my_start),
              cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED)) {
        // If SIGCONT arrived, phase is RUNNING -- we exit via the loop check.
        uint32_t phase = stop.phase.load(cpp::MemoryOrder::ACQUIRE);
        if (phase == PHASE_STOPPING || phase == PHASE_STOPPED) {
          // Resume the stop protocol from where the dead coordinator left off.
          // All threads already have STOP_REQUESTED (set by request_process_stop).
          // Re-alert any that haven't parked yet so they wake from kernel waits.
          registry_for_each([](ThreadLifecycle *node) -> bool {
            if (node->signal &&
                node->signal->stop_state.load(cpp::MemoryOrder::ACQUIRE) ==
                    STOP_STOPPED)
              return false;
            HANDLE h = registry_borrow_handle(node);
            if (h)
              windows::queue_special_user_apc(h, stop_checkpoint_apc);
            return false;
          }, my_tid);
          registry_alert_all(my_tid);

          // Hard-suspend any remaining stragglers that don't respond.
          stop_stragglers(stop, my_tid);

          // Transition to PHASE_STOPPED if still STOPPING.
          uint32_t stopping = PHASE_STOPPING;
          stop.phase.compare_exchange_strong(stopping, PHASE_STOPPED,
                                             cpp::MemoryOrder::RELEASE,
                                             cpp::MemoryOrder::RELAXED);
        }
      }
    }
  }

  // No decrement: the next cooperative_stop() atomically bumps generation
  // and resets count to 0 via a single store to ack_gen. Decrementing here
  // would race with resume, causing underflow on rapid stop→resume→stop.
  if (counted) {
    // POSIX: syscalls interrupted by stop/continue restart.
    signal_restart::push(state->restart, true);
  }
}

// ---------------------------------------------------------------------------
// resume_stopped_threads
// ---------------------------------------------------------------------------

void resume_stopped_threads() {
  auto &stop = g_pcb.signal_stop;

  // Clear coordinator. ack_gen is NOT reset here — cooperative_stop()
  // atomically bumps generation + resets count via a single store.
  // Threads exiting check_stop_request() don't decrement, so no
  // underflow risk.
  stop.coordinator.store(0, cpp::MemoryOrder::RELEASE);

  // Pass 1: set RUNNING on all threads, resume hard-suspended ones.
  registry_for_each([](ThreadLifecycle *node) -> bool {
    if (!node->signal)
      return false;
    unsigned char prev = node->signal->stop_state.exchange(
        STOP_RUNNING, cpp::MemoryOrder::ACQ_REL);
    if (prev == STOP_HARD_SUSPENDED) {
      signal_restart::push(node->signal->restart, true);
      registry_resume(node);
    }
    return false;
  }, 0);

  // Pass 2: batch-alert all cooperatively-parked threads via a single
  // NtAlertMultipleThreadByThreadId syscall.
  registry_alert_all(0);

  // Phase transition last — threads observe PHASE_RUNNING and know resume
  // is complete. coordinator already cleared above.
  stop.phase.store(PHASE_RUNNING, cpp::MemoryOrder::RELEASE);
}

// ---------------------------------------------------------------------------
// notify_parent_state_change
// ---------------------------------------------------------------------------

void notify_parent_state_change(int state_code) {
  auto &child = g_pcb.signal_child;
  if (!child.inherited_region)
    return;
  auto *region =
      static_cast<windows::SectionRegion *>(child.inherited_region);
  region->as<ChildStateBlock>()->state.store(state_code,
                                             cpp::MemoryOrder::RELEASE);
  if (child.inherited_event)
    ::NtSetEvent(child.inherited_event, nullptr);
}

// ---------------------------------------------------------------------------
// execute_default_action
// ---------------------------------------------------------------------------

void execute_default_action(int signum) {
  auto &child = g_pcb.signal_child;

  switch (signum) {
  // Ignore.
  case SIGCHLD:
  case SIGURG:
  case SIGWINCH:
    return;

  // Continue — resume cooperatively-parked threads and reverse any
  // kernel-level suspension.
  case SIGCONT:
    resume_stopped_threads();
    // Reverse state-change suspension if we have an active handle.
    // We only reverse our OWN suspension (created via NtChangeProcessState
    // during SIGSTOP). We do NOT call NtResumeProcess because we never call
    // NtSuspendProcess — doing so would unsafely reverse external suspensions
    // from debuggers, Process Explorer, or other diagnostic tools.
    if (child.process_state_change) {
      ::NtChangeProcessState(child.process_state_change, NtCurrentProcess(),
                             ProcessStateResume, nullptr, 0, 0);
      ::NtClose(child.process_state_change);
      child.process_state_change = nullptr;
    }
    notify_parent_state_change(CHILD_STATE_CONTINUED);
    return;

  // SIGSTOP — uncatchable, immediate. Kernel-level process suspension
  // via NtChangeProcessState for deadlock-free self-suspension.
  case SIGSTOP: {
    notify_parent_state_change(SIGSTOP);
    if (!child.process_state_change) {
      auto sc_oa = windows::internal_oa();
      ::NtCreateProcessStateChange(&child.process_state_change,
                                   PROCESS_STATE_ALL_ACCESS, &sc_oa,
                                   NtCurrentProcess(), 0);
    }
    if (child.process_state_change)
      ::NtChangeProcessState(child.process_state_change, NtCurrentProcess(),
                             ProcessStateSuspend, nullptr, 0, 0);
    return;
  }

  // SIGTSTP/SIGTTIN/SIGTTOU — cooperative three-phase stop.
  case SIGTSTP:
  case SIGTTIN:
  case SIGTTOU:
    cooperative_stop(signum);
    return;

  // Core — terminate with shell-convention exit code.
  case SIGQUIT:
  case SIGABRT:
  case SIGSEGV:
  case SIGBUS:
  case SIGFPE:
  case SIGILL:
  case SIGTRAP:
  // Terminate — all other signals.
  default:
    ::NtTerminateProcess(NtCurrentProcess(), 128 + signum);
    return;
  }
}

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
