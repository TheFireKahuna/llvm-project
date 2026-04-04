//===-- Dispatch engine (Layer 3) ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Signal dispatch state machine, handler invocation, and RT signal sorting.
//
// Bugs fixed:
//   #3: Alt-stack exhaustion — has_stack_space reads actual SP.
//   #4: SA_RESTART nesting — RestartState push/pop per level.
//   #5: Dispatch re-check ordering — explicit re-check protocol with
//       ACQUIRE loads and CAS DRAINING→IDLE.
//
// The dispatch loop is the single consumer of Layer 1's pending storage.
// It drains standard signals via drain_next_standard() and RT signals via
// RtBuckets (consumer-side per-signal sort of the single Vyukov inbox).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_engine.h"

#include "src/__support/OSUtil/windows/signal/control/process_control.h"
#include "hdr/signal_macros.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/struct_sigaction.h"
#include "hdr/types/ucontext_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/apc.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/alt_stack.h"
#include "src/__support/OSUtil/windows/signal/dispatch/handler_table.h"
#include "src/__support/OSUtil/windows/signal/dispatch/restart_state.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// Forward declarations for functions in signal_altstack.cpp / signal_delivery.cpp
// ---------------------------------------------------------------------------
extern void deliver_on_alt_stack(void *alt_stack_top, int signum,
                                 siginfo_t *info,
                                 const struct sigaction *action,
                                 ucontext_t *context);
extern void context_win32_to_ucontext(const CONTEXT *win_ctx, ucontext_t *uc);

namespace {

// Restore alt-stack state from autodisarm save or clear SS_ONSTACK.
// Shared by HandlerScope destructor and dispatch_pending longjmp recovery.
void restore_alt_stack(ThreadSignalState *state) {
  if (state->autodisarm_saved_sp) {
    state->alt_stack_sp = state->autodisarm_saved_sp;
    state->alt_stack_size = state->autodisarm_saved_size;
    state->alt_stack_flags = state->autodisarm_saved_flags;
    state->autodisarm_saved_sp = nullptr;
  } else {
    state->alt_stack_flags &= ~SS_ONSTACK;
  }
}

// ---------------------------------------------------------------------------
// Handler invocation (adapted from signal_delivery.cpp::invoke_handler)
// ---------------------------------------------------------------------------
// RAII guard for handler-scoped state. Ensures restart pop, mask restore,
// in_signal_handler, and alt-stack flags are cleaned up even if the handler
// exits via longjmp or C++ exception.
//
// For the longjmp case: dispatch_pending's entry-point recovery (depth
// reset to 0) is the primary defense. This guard is defense-in-depth that
// also protects against nested signal handlers within the dispatch loop.
struct HandlerScope {
  ThreadSignalState *state;
  cpp::Atomic<uint64_t> &preferred_thread;
  cpp::Atomic<uint64_t> &preferred_blocked;
  sigset_t old_mask;
  bool mask_saved;
  bool alt_stack_set;

  HandlerScope(ThreadSignalState *s, signal_state::SignalDispatchState &dispatch,
               int signum,
               const struct sigaction *action)
      : state(s), preferred_thread(dispatch.preferred_thread),
        preferred_blocked(dispatch.preferred_blocked), old_mask{},
        mask_saved(false), alt_stack_set(false) {
    if (!state)
      return;

    // Push restart state
    signal_restart::push(state->restart,
                         (action->sa_flags & SA_RESTART) != 0);

    // Block this signal (and sa_mask) during handler unless SA_NODEFER.
    if (!(action->sa_flags & SA_NODEFER)) {
      old_mask = state->blocked_signals;
      mask_saved = true;
      uint64_t cur = sigset_to_bits(state->blocked_signals);
      cur |= (1ULL << (signum - 1));
      cur |= sigset_to_bits(action->sa_mask);
      bits_to_sigset(cur, &state->blocked_signals);

      ThreadHandle self = current_thread_handle();
      if (self.is_valid() &&
          preferred_thread.load(cpp::MemoryOrder::RELAXED) == self.pack())
        preferred_blocked.store(cur, cpp::MemoryOrder::RELEASE);
    }

    state->in_signal_handler = true;
  }

  ~HandlerScope() {
    if (!state)
      return;

    // Pop restart state.
    signal_restart::pop(state->restart);

    if (alt_stack_set)
      restore_alt_stack(state);

    state->in_signal_handler = false;
    state->handler_ran = true;

    // Restore signal mask.
    if (mask_saved) {
      state->blocked_signals = old_mask;
      ThreadHandle self = current_thread_handle();
      if (self.is_valid() &&
          preferred_thread.load(cpp::MemoryOrder::RELAXED) == self.pack())
        preferred_blocked.store(sigset_to_bits(old_mask),
                                cpp::MemoryOrder::RELEASE);
    }
  }

  // Non-copyable.
  HandlerScope(const HandlerScope &) = delete;
  HandlerScope &operator=(const HandlerScope &) = delete;
};

// Returns true if the handler was invoked (or signal was handled via
// SIG_IGN/SIG_DFL). Returns false if delivery must be deferred because
// restart nesting is at max depth — caller must re-pend the signal.
bool invoke_handler_impl(ThreadSignalState *state, int signum, siginfo_t *info,
                         const struct sigaction *action) {
  if (!is_valid_signal(signum))
    return true; // Invalid signal — nothing to do, consider handled.

  // POSIX: SIGCONT generation unconditionally resumes, even if blocked/ignored.
  if (signum == SIGCONT)
    process_control::resume_stopped_threads();

  void (*handler)(int) = action->sa_handler;

  if (handler == SIG_IGN)
    return true;

  if (handler == SIG_DFL) {
    process_control::execute_default_action(signum);
    return true;
  }

  // Guard: if the restart stack is at max nesting depth, defer the signal.
  if (state && !signal_restart::can_push(state->restart))
    return false;

  auto &dispatch = g_pcb.signal_dispatch;

  // RAII scope: pushes restart, blocks mask, sets in_signal_handler.
  // Destructor pops restart, restores mask, clears flags — even if
  // the handler exits via longjmp or exception.
  HandlerScope scope(state, dispatch, signum, action);

  // SA_ONSTACK: deliver on alternate signal stack if configured.
  bool use_alt_stack = state && (action->sa_flags & SA_ONSTACK) &&
                       !(state->alt_stack_flags & SS_DISABLE) &&
                       !(state->alt_stack_flags & SS_ONSTACK);

  // Build ucontext_t from interrupted CONTEXT if available and handler
  // wants it (SA_SIGINFO). The interrupted_context pointer is valid only
  // during APC/VEH dispatch frames — it's a borrowed pointer, not a copy.
  ucontext_t uc;
  ucontext_t *uc_ptr = nullptr;
  if ((action->sa_flags & SA_SIGINFO) && state &&
      state->interrupted_context) {
    context_win32_to_ucontext(state->interrupted_context, &uc);
    uc_ptr = &uc;
  }

  if (use_alt_stack) {
    // Compute the 16-aligned top of the alt-stack BEFORE any disarming.
    auto *top = reinterpret_cast<void *>(
        (reinterpret_cast<ULONG_PTR>(state->alt_stack_sp) +
         state->alt_stack_size) &
        ~ULONG_PTR{0xF});

    // SS_AUTODISARM: save pre-modification alt-stack settings, then disarm.
    // Saved in ThreadSignalState (not stack-local) so longjmp recovery can
    // also restore them. restore_alt_stack() handles both paths.
    if (state->alt_stack_flags & SS_AUTODISARM) {
      state->autodisarm_saved_sp = state->alt_stack_sp;
      state->autodisarm_saved_size = state->alt_stack_size;
      state->autodisarm_saved_flags = state->alt_stack_flags;

      // Disarm: nested signals see SS_DISABLE and won't switch here.
      // SS_ONSTACK stays set — we ARE on it.
      state->alt_stack_sp = nullptr;
      state->alt_stack_size = 0;
      state->alt_stack_flags = SS_DISABLE | SS_ONSTACK;
    } else {
      state->alt_stack_flags |= SS_ONSTACK;
    }
    scope.alt_stack_set = true;

    deliver_on_alt_stack(top, signum, info, action, uc_ptr);
  } else {
    if (action->sa_flags & SA_SIGINFO)
      action->sa_sigaction(signum, info, uc_ptr);
    else
      handler(signum);
  }

  return true;
}

// Build a siginfo_t for an RT signal entry.
void build_rt_siginfo(siginfo_t *info, const SigqueueEntry *entry) {
  __builtin_memset(info, 0, sizeof(siginfo_t));
  info->si_signo = entry->si_signo;
  info->si_code = entry->si_code;
  info->si_value = entry->value;
  info->si_pid = entry->pid;
  info->si_uid = entry->uid;
}

// Build a siginfo_t for a standard signal from the PendingSet sidecars.
// si_code comes from the sidecar. si_pid/si_uid come from the process-wide
// CrossProcessSender array (for cross-process ALPC delivery) or are derived
// from the current process (for self-delivery). si_addr, populated by the
// VEH transport for hardware faults, carries the faulting VA (SIGSEGV/
// SIGBUS from access violations) or faulting instruction PC (SIGFPE/SIGILL/
// SIGTRAP and misc fault classes) — nullptr for software signals.
//
// siginfo_t's _kill and _sigfault union members alias (si_pid/si_uid share
// storage with si_addr), so the SI_USER/SI_TKILL and fault branches are
// mutually exclusive: write si_pid/si_uid on software signals, si_addr on
// faults. Kernel-sourced software signals (SI_KERNEL, SI_QUEUE, SI_TIMER,
// etc.) leave the union zeroed.
void build_standard_siginfo(siginfo_t *info, int signum, int si_code,
                            CrossProcessSender sender, void *fault_addr) {
  __builtin_memset(info, 0, sizeof(siginfo_t));
  info->si_signo = signum;
  info->si_code = si_code;
  if (si_code == SI_USER || si_code == SI_TKILL) {
    if (sender.pid != 0) {
      // Cross-process: use stored sender identity from ALPC transport.
      info->si_pid = sender.pid;
      info->si_uid = sender.uid;
    } else {
      // Self-delivery: derive from current process.
      info->si_pid = static_cast<pid_t>(NtCurrentProcessId());
      info->si_uid = static_cast<uid_t>(
          g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED));
    }
  } else if (fault_addr != nullptr) {
    info->si_addr = fault_addr;
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch notification via notify_word
// ---------------------------------------------------------------------------

namespace signal_dispatch {

void trigger(ThreadSignalState *state) {
  if (!state || !state->notify_word)
    return;

  // Set notify::SIGNAL on the owning thread's notification word.
  // signal_or: atomic OR + generation bump + conditional kernel alert
  // (fires NtAlertThreadByThreadId if the owner is in
  // ThreadLocalWord::wait_for_change). If the owner is running or in
  // UMWAIT/MWAITX, the cache-line write alone is sufficient.
  ThreadLocalWord::signal_or(state->notify_word, notify::SIGNAL);
}

// APC callback for process-directed signal dispatch. Queued to a target
// thread to force dispatch even when the thread is busy-spinning and not
// at a cooperative dispatch boundary. The special user APC fires at the
// next kernel-to-user transition (timer interrupt, ~15ms), analogous to
// how Linux delivers signals at the return-to-userspace boundary.
NTAPI static void dispatch_apc_callback(PVOID /*arg1*/, PVOID /*arg2*/,
                                        PVOID /*arg3*/) {
  ThreadSignalState *state = get_thread_state_noinit();
  if (state)
    dispatch_pending(state);
}

namespace {

void queue_dispatch_apc_to(HANDLE h) {
  windows::queue_special_user_apc(
      h, reinterpret_cast<PPS_APC_ROUTINE>(dispatch_apc_callback));
}

} // namespace

void trigger_any_thread() {
  // Wake at least one live thread so process-directed pending signals are
  // actually dispatched. The naive "trigger one, alert one, return" pattern
  // silently loses the wake when the chosen thread exits between trigger
  // and alert (Risk 5 in the signal-drop audit) — the pending bit stays
  // set in process_pending with nobody looking.
  //
  // This implementation loops until one of:
  //   (a) a candidate is confirmed alive AFTER we alerted it (its owner_tid
  //       is still non-zero), OR
  //   (b) the registry has no threads with signal state and non-zero
  //       owner_tid (the process is tearing down — no one to wake).
  //
  // Termination without a hard cap: every iteration either succeeds, or
  // rejects a candidate that's demonstrably gone. The registry shrinks
  // monotonically during teardown, so (b) is reached in bounded time even
  // in the pathological case where candidates die faster than we can
  // alert them. Normal operation: the first candidate is overwhelmingly
  // alive and we return after one pass.

  // Fast path: try the cached preferred thread. The cache stores a
  // packed ThreadHandle, NOT a raw lifecycle pointer — every load
  // re-resolves through registry_resolve so a recycled task_id slot is
  // rejected before we touch it.
  DWORD alert_tid = 0;
  {
    uint64_t packed =
        g_pcb.signal_dispatch.preferred_thread.load(cpp::MemoryOrder::ACQUIRE);
    ThreadHandle h = ThreadHandle::unpack(packed);
    if (h.is_valid()) {
      ThreadLifecycle *preferred = registry_resolve(h);
      // Single ACQUIRE load — avoids torn-read on lc->signal between
      // the if-check and the trigger() argument. A concurrent
      // deregister_thread_state can flip lc->signal to nullptr; with
      // two separate atomic loads the second could yield nullptr
      // even when the first read returned non-null, crashing inside
      // trigger().
      ThreadSignalState *preferred_sig =
          preferred ? preferred->signal.load(cpp::MemoryOrder::ACQUIRE)
                    : nullptr;
      if (preferred_sig) {
        trigger(preferred_sig);
        HANDLE th = registry_borrow_handle(preferred);
        if (th)
          queue_dispatch_apc_to(th);
        alert_tid = preferred->tid;
      }
    }
  }
  if (alert_tid) {
    alert_thread(alert_tid);
    // Post-alert liveness re-check. registry_resolve at the same
    // (tid, task_id) returning non-null means the lifecycle is still
    // registered and has either observed our notify::SIGNAL store
    // (TLW cache-line coherence) or will process our queued APC on
    // its next kernel-to-user transition.
    {
      uint64_t packed = g_pcb.signal_dispatch.preferred_thread.load(
          cpp::MemoryOrder::ACQUIRE);
      ThreadHandle h = ThreadHandle::unpack(packed);
      if (h.is_valid() && registry_resolve(h))
        return;
    }
    // Preferred died between trigger and confirm. Invalidate the cache
    // so future calls don't hit the stale entry; fall through to the
    // registry walk.
    uint64_t stale = g_pcb.signal_dispatch.preferred_thread.load(
        cpp::MemoryOrder::RELAXED);
    if (stale)
      g_pcb.signal_dispatch.preferred_thread.compare_exchange_strong(
          stale, 0, cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::RELAXED);
  }

  // Slow path: registry walk with retry. Each pass picks the first
  // thread with signal state, triggers it, alerts it, and re-checks
  // liveness. Returns when a candidate is confirmed alive post-alert,
  // or when the registry has no viable candidates left.
  for (;;) {
    alert_tid = 0;
    ThreadHandle found = ThreadHandle::invalid();
    (void)registry_for_each(
        [&](ThreadLifecycle *target) -> bool {
          // Single ACQUIRE load — same torn-read concern as the
          // preferred-fast-path above.
          ThreadSignalState *sig =
              target->signal.load(cpp::MemoryOrder::ACQUIRE);
          if (!sig)
            return false;
          trigger(sig);
          HANDLE th = registry_borrow_handle(target);
          if (th)
            queue_dispatch_apc_to(th);
          alert_tid = target->tid;
          found = ThreadHandle{target->tid, target->task_id};
          return true; // stop after first candidate
        },
        0);

    if (!alert_tid)
      return; // Registry exhausted — no dispatcher available.

    alert_thread(alert_tid);

    // Post-alert liveness check. registry_resolve matches on task_id
    // (which never recycles) and validates tid as defense-in-depth, so
    // a recycled TID with a different task_id correctly resolves to
    // null. Cache as preferred on success.
    if (registry_resolve(found)) {
      uint64_t expected = 0;
      g_pcb.signal_dispatch.preferred_thread.compare_exchange_strong(
          expected, found.pack(), cpp::MemoryOrder::RELEASE,
          cpp::MemoryOrder::RELAXED);
      return;
    }
    // Candidate died mid-flight. Retry with a fresh pick from registry.
  }
}

// ---------------------------------------------------------------------------
// Main dispatch loop
// ---------------------------------------------------------------------------

// RAII guard for the dispatch reentrancy flag. Clears `dispatching` on any
// exit path, including non-local exits via siglongjmp (RtlUnwindEx runs the
// Itanium personality, which invokes destructors during unwind). Without
// this, a handler that siglongjmp's out would leak `dispatching = true` and
// silently gate all future signal delivery to this thread.
struct DispatchReentryGuard {
  ThreadSignalState *state;
  ~DispatchReentryGuard() { state->dispatching = false; }
  DispatchReentryGuard(const DispatchReentryGuard &) = delete;
  DispatchReentryGuard &operator=(const DispatchReentryGuard &) = delete;
};

void dispatch_pending(ThreadSignalState *state) {
  if (!state)
    return;

  // Check stack space before entering dispatch.
  if (!signal_stack::can_dispatch(state))
    return;

  // Reentrancy guard — owner-only, no atomics needed.
  // If already dispatching (e.g., a signal handler called a function that
  // hits a dispatch boundary), the outer dispatch loop handles it.
  if (state->dispatching)
    return;
  state->dispatching = true;
  DispatchReentryGuard reentry_guard{state};

  // Note: siglongjmp recovery is handled entirely by RAII. HandlerScope's
  // destructor pops restart, clears in_signal_handler, and restores the
  // alt-stack during the unwind; DispatchReentryGuard above clears
  // `dispatching`. No imperative "recover stale state" pass is needed here.

  // Deferred re-fault reset from VEH.
  int reset_sig = state->refault_reset_signum;
  if (reset_sig > 0 && reset_sig < NSIG) {
    state->refault_reset_signum = 0;
    handler_table::deferred_reset(reset_sig);
  }

  // Clear VEH re-fault tracking. For synchronous exceptions (SIGSEGV etc.),
  // the faulting instruction re-executes immediately after VEH returns
  // CONTINUE_EXECUTION. If the same PC+code faults again before we reach
  // this point, VEH correctly passes through to the OS. By clearing here,
  // we ensure that unrelated faults at the same PC much later (after
  // handler invocation) are not falsely detected as re-faults.
  state->last_fault_pc = nullptr;
  state->last_fault_code = 0;

  // Consumer-side RT buckets — stack-local, zero persistent cost.
  RtBuckets buckets;

  // Process-wide pending set — stable reference, fetched once.
  auto &dispatch = g_pcb.signal_dispatch;

  // Outer loop: drain all pending signals, re-check, retry if new arrivals.
  // The loop exits only when:
  //   (a) nothing pending and no new notify::SIGNAL notification, or
  //   (b) stack exhausted — `stack_exhausted` propagates from the per-phase
  //       loops below, bypassing Phase 4 re-check so we fall straight to the
  //       cleanup block instead of spinning on stale bitmap bits.
  bool drain_complete = false;
  bool stack_exhausted = false;
  while (!drain_complete && !stack_exhausted &&
         signal_stack::can_dispatch(state)) {
    // ---- Phase 1: Absorb RT entries from inboxes into per-signal buckets ----
    // Absorb from both thread-local and process-wide pending sets. Process-
    // directed RT signals (from kill/sigqueue targeting the PID) are pended
    // to global->process_pending.inbox; thread-directed ones to state->pending.
    // Both must be drained here to ensure all RT signals are delivered.
    buckets.absorb(state->pending.inbox);
    buckets.absorb(dispatch.process_pending.inbox);

    uint64_t blocked_bits = sigset_to_bits(state->blocked_signals);
    uint32_t rt_mask = blocked_to_rt_mask(blocked_bits);

    // ---- Phase 2: Deliver standard signals (lowest first) ----
    // Drain from both thread-local and process-wide pending sets.
    // Process-directed standard signals are claimed via CAS from the
    // process-wide bitmap (multiple threads may race — CAS handles it).
    for (;;) {
      if (!signal_stack::can_dispatch(state)) {
        stack_exhausted = true;
        break;
      }

      // Try thread-local first (cheaper — single consumer, no contention).
      // Track which PendingSet the signal came from so we can read the
      // correct si_code sidecar slot after draining.
      PendingSet *source = &state->pending;
      int sig = signal_pending::drain_next_standard(*source, blocked_bits);
      // Then try process-wide (CAS contention possible but rare).
      if (sig == 0) {
        source = &dispatch.process_pending;
        sig = signal_pending::drain_next_standard(*source, blocked_bits);
      }
      if (sig == 0)
        break;

      // Read cross-process sender identity if drained from process-wide.
      // Clear after reading to prevent stale data on coalesced delivery.
      CrossProcessSender sender = {0, 0};
      if (source == &dispatch.process_pending) {
        sender = dispatch.process_sender[sig - 1];
        dispatch.process_sender[sig - 1] = {0, 0};
      }

      struct sigaction action = handler_table::read_and_consume(sig);
      siginfo_t info;
      // ACQUIRE loads pair with pend_standard's RELEASE stores, giving us
      // the (si_code, si_addr) tuple written together with the bit we
      // just drained.
      int si_code =
          source->standard_si_code[sig - 1].load(cpp::MemoryOrder::ACQUIRE);
      void *fault_addr = reinterpret_cast<void *>(
          source->standard_si_addr[sig - 1].load(cpp::MemoryOrder::ACQUIRE));
      build_standard_siginfo(&info, sig, si_code, sender, fault_addr);
      if (!invoke_handler_impl(state, sig, &info, &action)) {
        // Nesting overflow — re-pend so the signal is retried after an
        // outer handler returns and frees a restart stack slot. Preserve
        // fault_addr for fault signals so the deferred delivery still
        // reports the original faulting address.
        (void)signal_pending::pend_standard(state->pending, sig,
                                            info.si_code, fault_addr);
        break; // Stop draining — can't invoke any more handlers.
      }

      // Re-read blocked mask — handler may have changed it.
      blocked_bits = sigset_to_bits(state->blocked_signals);
      rt_mask = blocked_to_rt_mask(blocked_bits);
    }

    // Phase 2 may have tripped stack exhaustion — skip Phase 3/4 and fall
    // through to the cleanup block, same as the old `goto done`.
    if (stack_exhausted)
      break;

    // ---- Phase 3: Deliver RT signals (lowest-numbered first, FIFO within) ----
    for (;;) {
      if (!signal_stack::can_dispatch(state)) {
        stack_exhausted = true;
        break;
      }

      SigqueueEntry *entry = buckets.pop_lowest(rt_mask);
      if (!entry)
        break;

      int sig = entry->si_signo;
      struct sigaction action = handler_table::read_and_consume(sig);
      siginfo_t info;
      build_rt_siginfo(&info, entry);

      if (!invoke_handler_impl(state, sig, &info, &action)) {
        // Nesting overflow — re-push the RT entry into the inbox so it
        // is retried on the next drain pass. Don't free the entry.
        signal_pending::pend_rt(state->pending, entry);
        break; // Stop draining.
      }
      sigqueue_free(entry);

      // Re-read blocked mask.
      blocked_bits = sigset_to_bits(state->blocked_signals);
      rt_mask = blocked_to_rt_mask(blocked_bits);
    }

    // Phase 3 may have tripped stack exhaustion — skip Phase 4 re-check
    // (outer `while` guard will exit on next iteration).
    if (stack_exhausted)
      break;

    // ---- Phase 4: Re-check ----
    // Check both thread-local and process-wide pending sets, plus the
    // inboxes, plus the notify_word. New signals may have arrived during
    // handler execution (APC, VEH, cross-thread signal_or).
    //
    // For standard signals, the bitmap bit is authoritative (cleared by
    // drain_next_standard). For RT signals, the bitmap bit is a presence
    // hint that may become stale after delivery — the inbox + buckets are
    // the source of truth. So we check standard bits via the bitmap, and
    // RT signals via inbox emptiness + bucket state. This prevents infinite
    // looping on stale RT presence bits.
    //
    // Ordering: clear notify::SIGNAL *before* loading the pending bitmaps.
    // The producer side (signal_or + pend_*) sets pending first, then
    // notify::SIGNAL. By clearing notify first and loading pending after,
    // we establish this guarantee:
    //   - Any producer whose pend_* is visible after our clear: we observe
    //     its pending bits in the loads below.
    //   - Any producer that completes pend_* + signal_or strictly after our
    //     pending load: its notify::SIGNAL store comes after our clear, so
    //     a later dispatch_pending entry will see it and re-drain.
    // Without the clear-first ordering, a producer could set pending+notify
    // between our pending load and our clear, and we would wipe the
    // notification of a signal we hadn't observed.
    //
    // clear_flags is ACQ_REL by contract (see ThreadLocalWord::clear_flags):
    // the AQ tag on the RMW prevents the subsequent ACQUIRE loads of the
    // pending bitmaps from being hoisted above the clear on AArch64, and
    // the RL tag publishes the clear to producers. No external fence needed.
    if (state->notify_word)
      state->notify_word->clear_flags(notify::SIGNAL);

    uint64_t recheck_blocked = sigset_to_bits(state->blocked_signals);
    uint64_t thread_pending =
        state->pending.standard.load(cpp::MemoryOrder::ACQUIRE);
    uint64_t proc_pending =
        dispatch.process_pending.standard.load(cpp::MemoryOrder::ACQUIRE);
    uint64_t all_pending = thread_pending | proc_pending;
    bool rt_arrived = !state->pending.inbox.empty() ||
                      !dispatch.process_pending.inbox.empty();

    if ((all_pending & ~recheck_blocked & STANDARD_SIGNALS_MASK) != 0 ||
        rt_arrived ||
        buckets.has_deliverable(blocked_to_rt_mask(recheck_blocked))) {
      continue; // New signals — loop back.
    }

    drain_complete = true;
  }

  // Re-push any RT entries that were absorbed from the inbox into buckets
  // but could not be delivered (because they were blocked). Without this,
  // the entries are leaked — removed from the inbox but never returned —
  // leaving the bitmap bit set with no inbox entry. The next dispatch_pending
  // would loop forever: absorb drains an empty inbox, buckets are empty,
  // but the stale bitmap bit keeps the re-check loop spinning.
  if (buckets.bitmap)
    buckets.repush_remaining(state->pending);

  // `dispatching` is cleared by DispatchReentryGuard's destructor on function
  // exit — including non-local exits via siglongjmp.

  // Note: stale RT presence bits may remain in the standard bitmaps after
  // all inbox entries have been consumed. This is benign because Phase 4's
  // re-check uses STANDARD_SIGNALS_MASK (not SIGNAL_BITS_MASK), so stale
  // RT bits cannot cause the drain loop to spin. A stale bit at worst
  // triggers one extra dispatch pass at entry (Phase 1 absorbs an empty
  // inbox, Phase 3 finds empty buckets). Attempting to clear stale bits
  // would race with concurrent producers pushing new RT entries, potentially
  // clearing a freshly-set presence bit and making the new entry invisible.

  // Check for pending stop request (cooperative SIGSTOP/SIGTSTP).
  process_control::check_stop_request(state);
}

bool should_restart_syscall(ThreadSignalState *state) {
  if (!state)
    return false;

  // Dispatch any pending signals first.
  dispatch_pending(state);

  // Read from the restart stack, not a one-shot scalar.
  return signal_restart::should_restart(state->restart);
}

} // namespace signal_dispatch
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
