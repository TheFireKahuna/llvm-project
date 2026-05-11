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
// Per-thread stop notification uses the notify::STOP bit in the owning
// ThreadLifecycle's ThreadLocalWord (notify_word). The coordinator sets the
// bit via signal_or(); each thread checks it via test_flags() — one MOV +
// TEST, zero atomics on the fast path.
//
// Coordinator death recovery — three independent paths:
//
//   Primary: on_coordinator_exit() runs synchronously in the dying
//     thread's lifecycle_cleanup (TLS callback). Covers normal exit,
//     pthread_cancel, SEH faults. No external dependencies.
//
//   Backup: reactor WCP watch on the coordinator's thread handle,
//     registered by a NON-coordinator parked thread. Covers external
//     NtTerminateThread (skips TLS callbacks). WCP guarantee: if the
//     handle is already signaled at registration time, the completion
//     fires immediately — no gap between coordinator death and watch
//     registration, because the registering thread is alive.
//
//   Fallback: immediate COORD_DEAD broadcast if watch() fails (OOM,
//     reactor shutdown, handle lookup failure).
//
// All paths broadcast notify::COORD_DEAD via signal_or() to all parked
// threads. Parked threads wake from wait_for_change (hardware monitor or
// Dekker alert) and the first to CAS coordinator_tid steals the role.
// No polling, no arbitrary timeout.
//
// Three-phase stop protocol:
//   Phase 1: signal_or(notify::STOP) + APC + alert
//   Phase 2: Second APC round for kernel-wait threads
//   Phase 3a: Suspend-APC-Resume (kernel transition fires APC)
//   Phase 3b: Hard-suspend (last resort)
//
// notify_word access convention:
//   Coordinator paths (request_process_stop, stop_stragglers, resume)
//   access ThreadLifecycle::notify_word directly via node->notify_word
//   — the ThreadLocalWord value embedded in the lifecycle struct.
//
//   Owner paths (check_stop_request, sigsuspend/sigtimedwait) access
//   via ThreadSignalState::notify_word — a back-pointer set to
//   &lc->notify_word during register_thread_state(). Both reference
//   the same object; the indirection exists so dispatch code can reach
//   the word from just the signal state pointer.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/control/process_control.h"

#include "src/__support/OSUtil/windows/debug/crash_handler.h"
#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/apc.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/OSUtil/windows/signal/dispatch/restart_state.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_context_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_local_word.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace process_control {

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
// Coordinator death watch — reactor callback
// ---------------------------------------------------------------------------
//
// Fires on a reactor drain thread when the coordinator's thread handle
// enters the signaled state (the coordinator thread exited). The callback
// sets notify::COORD_DEAD on every parked thread's notify_word via
// signal_or(). This writes the cache line → hardware-monitored threads
// wake in ~100ns. Threads in kernel sleep wake via the Dekker alert.
//
// The first parked thread to wake CAS-steals the coordinator TID and
// finishes the stop protocol.

static void coordinator_death_cb(void * /*context*/, NTSTATUS /*status*/,
                                 ULONG_PTR /*information*/) {
  auto &stop = g_pcb.signal_stop;

  // Verify we're still in a stop — SIGCONT may have already cleaned up.
  uint32_t phase = stop.phase.load(cpp::MemoryOrder::ACQUIRE);
  if (phase != PHASE_STOPPING && phase != PHASE_STOPPED)
    return;

  // Broadcast COORD_DEAD to all threads. signal_or writes the cache
  // line (hardware monitor wake) + bumps generation + fires kernel
  // alert if the owner is in wait_for_change kernel sleep.
  registry_for_each([](ThreadLifecycle *node) -> bool {
    ThreadLocalWord::signal_or(&node->notify_word, notify::COORD_DEAD);
    return false;
  }, 0);
}

// ---------------------------------------------------------------------------
// Synchronous coordinator death broadcast (primary path)
// ---------------------------------------------------------------------------
//
// Called from lifecycle_cleanup on the dying thread itself. If this thread
// is the coordinator, broadcast COORD_DEAD to all parked threads. Runs
// before signal deregistration — parked threads can still see us in the
// registry for the CAS-steal. This path covers normal exit, pthread_cancel,
// and SEH-handled faults. The reactor WCP watch is backup for external
// NtTerminateThread which skips TLS callbacks entirely.

void on_coordinator_exit(uint32_t tid) {
  auto &stop = g_pcb.signal_stop;

  // Fast path: not the coordinator — single RELAXED load, ~1 cycle.
  if (stop.coordinator_tid.load(cpp::MemoryOrder::RELAXED) != tid)
    return;

  // Verify with ACQUIRE to synchronize with cooperative_stop's RELEASE.
  if (stop.coordinator_tid.load(cpp::MemoryOrder::ACQUIRE) != tid)
    return;

  uint32_t phase = stop.phase.load(cpp::MemoryOrder::ACQUIRE);
  if (phase != PHASE_STOPPING && phase != PHASE_STOPPED)
    return;

  // Same broadcast as coordinator_death_cb — wake all parked threads.
  registry_for_each([](ThreadLifecycle *node) -> bool {
    ThreadLocalWord::signal_or(&node->notify_word, notify::COORD_DEAD);
    return false;
  }, 0);
}

// ---------------------------------------------------------------------------
// Register / unregister the coordinator death watch
// ---------------------------------------------------------------------------

static void register_death_watch(SignalStopState &stop, DWORD coordinator_tid) {
  HANDLE h = nullptr;
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    if (node->tid == coordinator_tid) {
      h = registry_borrow_handle(node);
      return true;
    }
    return false;
  }, 0);

  if (!h) {
    // Handle lookup failed (thread deregistered between us storing
    // coordinator_tid and scanning). Broadcast COORD_DEAD immediately
    // so parked threads can elect a new coordinator rather than livelocking.
    coordinator_death_cb(nullptr, STATUS_SUCCESS, 0);
    return;
  }

  stop.coordinator_handle = h;
  // NT WCP guarantee: if the handle is already signaled (coordinator died
  // between Phase 3 and this call), NtAssociateWaitCompletionPacket posts
  // the completion immediately. No window for undetected death.
  stop.death_watch =
      internal::reactor::watch(h, coordinator_death_cb, nullptr);

  // watch() can fail on OOM or reactor shutdown. Without the WCP, a dead
  // coordinator would go undetected → permanent livelock. Fall back to
  // an immediate broadcast so parked threads can recover.
  if (!stop.death_watch.valid())
    coordinator_death_cb(nullptr, STATUS_SUCCESS, 0);
}

static void unregister_death_watch(SignalStopState &stop) {
  if (stop.death_watch.valid()) {
    // unwatch() spin-waits until any in-flight callback completes.
    // After return, no callback can fire — safe to clear fields.
    internal::reactor::unwatch(stop.death_watch);
    stop.death_watch = internal::reactor::INVALID_TOKEN;
  }
  stop.coordinator_handle = nullptr;
}

// ---------------------------------------------------------------------------
// Stop checkpoint APC
// ---------------------------------------------------------------------------
//
// Injected into each thread during the stop protocol. Fires at kernel→user
// transitions (syscall return, context switch, page fault). Calls
// check_stop_request() which parks the thread cooperatively.

NTAPI static void stop_checkpoint_apc(PVOID, PVOID, PVOID) {
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

  // Thread may have self-parked between our caller's check and the suspend.
  if (auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE)) {
    if (sig->stop_parked.load(cpp::MemoryOrder::RELAXED)) {
      registry_resume(node);
      return true;
    }
  }

  // Queue before resume — APC must be pending when the kernel→user
  // transition fires, otherwise the thread runs without seeing the stop.
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

  auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE);
  if (!sig) {
    registry_resume(node);
    return;
  }

  // Already cooperatively parked — resume so the APC can fire normally.
  // Hard-suspending a parked thread would leave it stuck: resume_stopped_threads
  // wouldn't know to NtResumeThread it (hard_suspended not set), and it
  // can't wake from wait_for_change while kernel-suspended.
  if (sig->stop_parked.load(cpp::MemoryOrder::RELAXED)) {
    registry_resume(node);
    return;
  }

  // SIGCONT may have arrived between our Phase 3a attempt and this suspend.
  // The thread is frozen — it can't clear its own STOP bit — so a cleared
  // bit means resume_stopped_threads ran. Don't leave it hard-suspended
  // when the stop is already over.
  if (sig->notify_word &&
      !sig->notify_word->test_flags(notify::STOP)) {
    registry_resume(node);
    return;
  }

  // Leave the thread kernel-suspended. resume_stopped_threads will
  // NtResumeThread it when SIGCONT arrives.
  sig->hard_suspended.store(true, cpp::MemoryOrder::RELAXED);
}

// ---------------------------------------------------------------------------
// Phase 3: Stop stragglers surviving both APC rounds
// ---------------------------------------------------------------------------

static void stop_stragglers(signal_state::SignalStopState &stop,
                            DWORD skip_tid) {
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    if (auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE))
      if (sig->stop_parked.load(cpp::MemoryOrder::RELAXED))
        return false;
    suspend_apc_resume(node);
    return false;
  }, skip_tid);

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

  registry_for_each([&](ThreadLifecycle *node) -> bool {
    if (auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE))
      if (sig->stop_parked.load(cpp::MemoryOrder::RELAXED))
        return false;
    hard_suspend_one_thread(node);
    return false;
  }, skip_tid);
}

// ---------------------------------------------------------------------------
// Request all threads to stop
// ---------------------------------------------------------------------------

static void request_process_stop(DWORD self_tid) {
  // signal_or wakes threads in wait_for_change (hardware monitor or
  // Dekker alert). APCs cover threads in other kernel waits (I/O, futex,
  // sigsuspend) where the notify_word isn't the sleep primitive.
  registry_for_each([&](ThreadLifecycle *node) -> bool {
    ThreadLocalWord::signal_or(&node->notify_word, notify::STOP);
    if (node->tid != self_tid) {
      HANDLE h = registry_borrow_handle(node);
      if (h)
        windows::queue_special_user_apc(h, stop_checkpoint_apc);
    }
    return false;
  }, 0); // don't skip self — we need our own STOP bit set for self-park

  // Batch-alert catches threads in raw NtWaitForAlertByThreadId waits
  // that signal_or's per-thread alert can't reach (I/O ring, futex).
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
    return;

  DWORD self_tid = NtCurrentThreadId();
  uint32_t target = registry_live_count();

  stop.coordinator_tid.store(self_tid, cpp::MemoryOrder::RELEASE);

  // Packed gen+count store: threads from a prior stop cycle that are still
  // in check_stop_request will fail their ack CAS (generation mismatch)
  // and re-ack the new cycle. No stale ack carry-over.
  uint32_t new_gen =
      ack_gen_generation(stop.ack_gen.load(cpp::MemoryOrder::RELAXED)) + 1;
  stop.ack_gen.store(pack_ack_gen(new_gen, 0), cpp::MemoryOrder::RELEASE);
  stop.target_count.store(target, cpp::MemoryOrder::RELEASE);

  bool all_parked = (target <= 1);

  // Phase 1: set notify::STOP + inject APCs + alert all threads.
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
      auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE);
      if (sig && sig->stop_parked.load(cpp::MemoryOrder::RELAXED))
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

  // RELEASE: all stop setup (ack_gen, APCs, hard-suspends) visible
  // before any thread observes PHASE_STOPPED.
  stop.phase.store(PHASE_STOPPED, cpp::MemoryOrder::RELEASE);
  notify_parent_state_change(signum);

  // Death watch registration is handled by parked non-coordinator threads
  // (see check_stop_request). The coordinator does NOT self-register —
  // self-registration has an atomic gap: if NtTerminateThread kills us
  // mid-registration, the WCP is never created and parked threads livelock.
  // Non-coordinator registration eliminates this gap entirely: the
  // registering thread is alive, and WCP's "already-signaled fires
  // immediately" guarantee covers the case where we're already dead.
  //
  // death_watch_registered is NOT reset here — it's already false from
  // init or the prior cycle's resume_stopped_threads. Storing false here
  // would race with a fast non-coordinator that already CAS'd it to true
  // and registered the watch during Phase 1/2/3, causing a double WCP
  // registration and leaking the first reactor slot.

  ThreadSignalState *self = get_thread_state_noinit();
  if (self)
    check_stop_request(self);
}

// ---------------------------------------------------------------------------
// check_stop_request — per-thread cooperative park
// ---------------------------------------------------------------------------
//
// Fast-path: test_flags(notify::STOP) — one MOV + TEST + Jcc, ~2 cycles,
// zero atomics. Only enters the park loop when the STOP bit is set.
//
// Park: ThreadLocalWord::wait_for_change() — two-phase adaptive wait:
//   Phase 1: UMWAIT/MWAITX hardware address monitor. Resume's
//     signal_clear_bits writes the cache line → ~100ns wake.
//   Phase 2: NtWaitForAlertByThreadId kernel sleep with Dekker protocol.
//     signal_clear_bits fires NtAlert if park_state_ == KERNEL_PARKED.
//
// Death watch: the first non-coordinator thread to park registers the
// WCP watch on the coordinator's handle via CAS on death_watch_registered.
// This eliminates the self-registration atomic gap: the registering thread
// is alive, and WCP's "already-signaled fires immediately" guarantee means
// a coordinator that died before registration is detected instantly.
//
// Coordinator death: three independent paths set notify::COORD_DEAD:
//   (1) on_coordinator_exit — synchronous, in the dying thread's cleanup
//   (2) reactor WCP watch — async, registered by a parked non-coordinator
//   (3) immediate broadcast — fallback if watch() fails
// Any one is sufficient. The first thread to CAS coordinator_tid steals.
//
// Reentrancy: stop_checkpoint_apc may fire during wait_for_change (APCs
// execute at kernel→user transitions). The APC calls check_stop_request,
// which sees stop_parked == true and returns immediately.

void check_stop_request(ThreadSignalState *state) {
  if (!state || !state->notify_word)
    return;

  ThreadLocalWord *nw = state->notify_word;

  // Fast path: no active stop — one RELAXED load, ~2 cycles.
  if (LIBC_LIKELY(!nw->test_flags(notify::STOP)))
    return;

  // Reentrancy guard: stop_checkpoint_apc may fire during wait_for_change
  // below. wait_for_change is NOT reentrant on the same ThreadLocalWord.
  if (state->stop_parked.load(cpp::MemoryOrder::RELAXED))
    return;

  auto &stop = g_pcb.signal_stop;
  bool counted = false;
  uint32_t my_gen =
      ack_gen_generation(stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE));

  while (true) {
    if (!nw->test_flags(notify::STOP))
      break;

    // Rapid stop→resume→stop can start a new cycle while we're still in
    // the park loop from the previous one. Reset counted so we re-ack
    // the new generation instead of silently sitting on a stale count.
    uint64_t ag = stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE);
    uint32_t cur_gen = ack_gen_generation(ag);
    if (counted && cur_gen != my_gen) {
      counted = false;
      my_gen = cur_gen;
    }

    // RELEASE: on weakly-ordered architectures (AArch64), a RELAXED store
    // before an ACQ_REL CAS is not guaranteed to be in the CAS's release
    // sequence. RELEASE here ensures the coordinator's ACQUIRE of ack_gen
    // sees stop_parked == true. On x86 (TSO), RELEASE = plain MOV — free.
    // Also doubles as the reentrancy guard for stop_checkpoint_apc.
    state->stop_parked.store(true, cpp::MemoryOrder::RELEASE);

    if (!counted) {
      // Packed CAS: validates generation AND bumps count atomically.
      // If another cycle started between our read and here, the CAS
      // fails on generation mismatch — no double-ack, no TOCTOU.
      ag = stop.ack_gen.load(cpp::MemoryOrder::ACQUIRE);
      cur_gen = ack_gen_generation(ag);
      uint64_t desired = pack_ack_gen(cur_gen, ack_gen_count(ag) + 1);
      if (stop.ack_gen.compare_exchange_strong(ag, desired,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE)) {
        counted = true;
        my_gen = cur_gen;
      }
    }

    // Non-coordinator threads race to register the WCP death watch.
    // Only one thread wins the CAS; losers skip (~5 cycles). The
    // registering thread is alive, so no self-registration atomic gap.
    // WCP guarantee: if the coordinator is already dead (handle signaled),
    // the completion fires immediately — zero detection latency.
    uint32_t my_tid = NtCurrentThreadId();
    if (my_tid != stop.coordinator_tid.load(cpp::MemoryOrder::RELAXED)) {
      bool expected_reg = false;
      if (stop.death_watch_registered.compare_exchange_strong(
              expected_reg, true, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED)) {
        register_death_watch(
            stop, stop.coordinator_tid.load(cpp::MemoryOrder::ACQUIRE));
      }
    }

    // No timeout — wakes only on value_ change (resume clears STOP,
    // reactor sets COORD_DEAD) or spurious (CANCEL/SIGNAL bit flip).
    // Interruptible=false: APCs fire during kernel sleep but the APC
    // callback's reentrancy guard (stop_parked) makes them no-ops.
    uint32_t expected = nw->read();
    nw->wait_for_change</*Interruptible=*/false>(expected);

    if (!nw->test_flags(notify::STOP))
      break;

    // Reactor or lifecycle_cleanup set COORD_DEAD — coordinator exited.
    if (nw->test_flags(notify::COORD_DEAD)) {
      nw->clear_flags(notify::COORD_DEAD);

      // CAS-steal: only one thread wins. Losers re-loop and re-park.
      uint32_t old_tid =
          stop.coordinator_tid.load(cpp::MemoryOrder::ACQUIRE);
      if (old_tid != 0 && old_tid != my_tid &&
          stop.coordinator_tid.compare_exchange_strong(
              old_tid, my_tid, cpp::MemoryOrder::ACQ_REL,
              cpp::MemoryOrder::RELAXED)) {

        // Re-check phase at each step: SIGCONT may have raced with
        // coordinator death and already called resume_stopped_threads.
        // Continuing after PHASE_RUNNING would double-resume threads.
        uint32_t phase = stop.phase.load(cpp::MemoryOrder::ACQUIRE);
        if (phase != PHASE_STOPPING && phase != PHASE_STOPPED)
          continue; // SIGCONT already handled — just re-park or exit.

        // Tear down the dead coordinator's watch. unregister_death_watch
        // is safe to double-call (idempotent) and spin-waits until any
        // in-flight callback completes.
        unregister_death_watch(stop);

        // Bail if SIGCONT arrived between unregister and here.
        if (stop.phase.load(cpp::MemoryOrder::ACQUIRE) == PHASE_RUNNING)
          continue;

        // Reset the registration flag so a CAS-steal loser (or a
        // re-parking thread) registers the WCP watch on us — the new
        // coordinator. Same cross-thread pattern: no self-registration.
        stop.death_watch_registered.store(false, cpp::MemoryOrder::RELEASE);

        // Re-alert threads the dead coordinator never reached.
        registry_for_each([](ThreadLifecycle *node) -> bool {
          auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE);
          if (sig && sig->stop_parked.load(cpp::MemoryOrder::RELAXED))
            return false;
          HANDLE h = registry_borrow_handle(node);
          if (h)
            windows::queue_special_user_apc(h, stop_checkpoint_apc);
          return false;
        }, my_tid);
        registry_alert_all(my_tid);

        // Bail if SIGCONT arrived during APC broadcast.
        if (stop.phase.load(cpp::MemoryOrder::ACQUIRE) == PHASE_RUNNING)
          continue;

        stop_stragglers(stop, my_tid);

        uint32_t stopping = PHASE_STOPPING;
        stop.phase.compare_exchange_strong(stopping, PHASE_STOPPED,
                                           cpp::MemoryOrder::RELEASE,
                                           cpp::MemoryOrder::RELAXED);
      }
    }
  }

  state->stop_parked.store(false, cpp::MemoryOrder::RELAXED);

  if (counted)
    signal_restart::push(state->restart, true);
}

// ---------------------------------------------------------------------------
// resume_stopped_threads
// ---------------------------------------------------------------------------

void resume_stopped_threads() {
  auto &stop = g_pcb.signal_stop;

  // Must unwatch before clearing coordinator state. unwatch() spin-waits
  // until any in-flight death callback completes — without this, the
  // callback could race with our field clears and signal_or a stale TID.
  unregister_death_watch(stop);
  stop.death_watch_registered.store(false, cpp::MemoryOrder::RELAXED);
  stop.coordinator_tid.store(0, cpp::MemoryOrder::RELEASE);

  // signal_clear_bits: clears bits + gen bump + Dekker alert if the
  // owner is in wait_for_change kernel sleep. Threads in hardware-
  // monitor sleep wake from the cache-line write alone.
  registry_for_each([](ThreadLifecycle *node) -> bool {
    ThreadLocalWord::signal_clear_bits(&node->notify_word,
                                       notify::STOP | notify::COORD_DEAD);

    auto *sig = node->signal.load(cpp::MemoryOrder::ACQUIRE);
    if (!sig)
      return false;

    // Hard-suspended threads can't push their own restart state (they're
    // frozen in the kernel), so we do it on their behalf before resuming.
    if (sig->hard_suspended.load(cpp::MemoryOrder::RELAXED)) {
      signal_restart::push(sig->restart, true);
      sig->hard_suspended.store(false, cpp::MemoryOrder::RELAXED);
      registry_resume(node);
    }
    return false;
  }, 0);

  // Belt-and-suspenders: threads between APC fire and check_stop_request
  // entry are in a raw NtWaitForAlertByThreadId (not wait_for_change).
  // signal_clear_bits can't wake them — this batch alert can.
  registry_alert_all(0);

  // Phase transition last — readers use ACQUIRE to see all prior cleanup.
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

namespace {

// Map a fatal POSIX signal to the canonical Windows NTSTATUS used when
// re-raising via NtRaiseException for software-derived signals (no original
// EXCEPTION_RECORD available). The resulting exit code that the kernel
// plants in the process's ExitStatus flows back through the parent's
// ntstatus_to_signal table (child_table.cpp) to the same POSIX signum —
// round-trip-fidelity for waitpid / WIFSIGNALED / WTERMSIG.
//
// Hardware-derived signals do not pass through this map: fatal_raise_signal
// uses the original EXCEPTION_RECORD's ExceptionCode unchanged so minidumps
// carry the true fault classification, not a synthetic.
DWORD signal_to_ntstatus(int signum) {
  switch (signum) {
  case SIGSEGV: return EXCEPTION_ACCESS_VIOLATION;       // 0xC0000005
  case SIGBUS:  return EXCEPTION_DATATYPE_MISALIGNMENT;  // 0x80000002
  case SIGFPE:  return EXCEPTION_INT_DIVIDE_BY_ZERO;     // 0xC0000094
  case SIGILL:  return EXCEPTION_ILLEGAL_INSTRUCTION;    // 0xC000001D
  case SIGTRAP: return EXCEPTION_BREAKPOINT;             // 0x80000003
  case SIGABRT: return 0x40000015u;                       // STATUS_FATAL_APP_EXIT
  default:      return 0x40000015u;
  }
}

// Re-raise a fatal signal as a structured Windows exception so the
// debugger, WER, and the OS unhandled-exception filter chain can act on
// it. Replaces the legacy NtTerminateProcess(128+signum) silent terminate
// for the SIG_DFL=core+terminate signals.
//
// Two paths:
//   - Hardware-derived (state has a saved record + context paired with
//     this signum, set by the VEH transport): replay the original record
//     verbatim with the original ContextRecord. Minidumps capture the
//     actual fault site; debuggers stop where the fault originally
//     occurred; parent's GetExitCodeProcess yields the real fault NTSTATUS.
//   - Software-derived (raise(), abort(), console ctrl): synthesize a
//     record carrying the canonical NTSTATUS for the signum and capture
//     the current CPU state. The fault site is execute_default_action's
//     caller — close enough for software signals.
//
// The fatal_raise_in_progress sentinel must be set BEFORE NtRaiseException:
// the kernel re-enters master_veh_handler on this thread for the new
// exception, and the sentinel must already be visible there to short-
// circuit our filter chain. Otherwise signal_veh_transport would re-pend
// the signal and dispatch in a loop. Set once, never cleared — the
// process is dying.
[[noreturn]]
void fatal_raise_signal(int signum, ThreadSignalState *state) {
  EXCEPTION_RECORD synth_rec;
  CONTEXT captured;
  EXCEPTION_RECORD *rec;
  CONTEXT *ctx;

  if (state && state->interrupted_record && state->interrupted_context &&
      state->interrupted_signum == signum) {
    // Hardware-derived: replay the original record + context unchanged.
    // Force EXCEPTION_NONCONTINUABLE so any handler that survives our
    // sentinel skip cannot return EXCEPTION_CONTINUE_EXECUTION and re-run
    // the faulting instruction.
    state->interrupted_record->ExceptionFlags |= EXCEPTION_NONCONTINUABLE;
    rec = state->interrupted_record;
    ctx = state->interrupted_context;
  } else {
    // Software-derived: synthesize a record at our caller's PC and
    // capture the current CPU state.
    __builtin_memset(&synth_rec, 0, sizeof(synth_rec));
    synth_rec.ExceptionCode    = signal_to_ntstatus(signum);
    synth_rec.ExceptionFlags   = EXCEPTION_NONCONTINUABLE;
    synth_rec.ExceptionAddress = __builtin_return_address(0);
    synth_rec.NumberParameters = 0;
    ::RtlCaptureContext(&captured);
    rec = &synth_rec;
    ctx = &captured;
  }

  if (state)
    state->fatal_raise_in_progress = true;

  // FirstChance=FALSE: the application's only "handler" was the SIG_DFL
  // terminate path now executing here — first-chance dispatch has nothing
  // useful to do. Skip directly to second chance so the unhandled-
  // exception filter and WER run sooner.
  ::NtRaiseException(rec, ctx, /*FirstChance=*/FALSE);

  // Defensive — second-chance NONCONTINUABLE never returns. If somehow
  // it does, fall back to a hard terminate so the process still dies and
  // the parent observes a meaningful exit code.
  ::NtTerminateProcess(NtCurrentProcess(), rec->ExceptionCode);
  __builtin_trap();
}

} // namespace

void execute_default_action(int signum) {
  auto &child = g_pcb.signal_child;

  switch (signum) {
  case SIGCHLD:
  case SIGURG:
  case SIGWINCH:
    return;

  case SIGCONT:
    resume_stopped_threads();
    if (child.process_state_change) {
      ::NtChangeProcessState(child.process_state_change, NtCurrentProcess(),
                             ProcessStateResume, nullptr, 0, 0);
      ::NtClose(child.process_state_change);
      child.process_state_change = nullptr;
    }
    notify_parent_state_change(CHILD_STATE_CONTINUED);
    return;

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

  case SIGTSTP:
  case SIGTTIN:
  case SIGTTOU:
    cooperative_stop(signum);
    return;

  case SIGABRT:
  case SIGSEGV:
  case SIGBUS:
  case SIGFPE:
  case SIGILL:
  case SIGTRAP:
    // Write the in-process backtrace to stderr first so users without a
    // debugger / without WER configured still see a fault report. Then
    // re-raise as a structured exception so debuggers stop, WER captures
    // a minidump, and the parent's waitpid sees the right NTSTATUS.
    internal::crash_backtrace(signum);
    fatal_raise_signal(signum, get_thread_state_noinit());
    __builtin_unreachable();

  case SIGQUIT:
    // SIGQUIT keeps the Cygwin/Unix 128+sig encoding rather than G1's
    // NtRaiseException. No Microsoft NTSTATUS fits "user-requested core
    // dump", and a libc customer-bit code would be opaque to shell
    // tooling that already speaks 128+sig — meanwhile our parent
    // round-trips Cygwin's 131 for free via the existing 128+sig arm.
    internal::crash_backtrace(signum);
    ::NtTerminateProcess(NtCurrentProcess(), 128 + signum);
    return;

  default:
    ::NtTerminateProcess(NtCurrentProcess(), 128 + signum);
    return;
  }
}

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
