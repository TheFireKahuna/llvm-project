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
  cpp::Atomic<ThreadLifecycle *> &preferred_thread;
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

      auto *cur_lc = get_current_lifecycle();
      if (cur_lc &&
          preferred_thread.load(cpp::MemoryOrder::RELAXED) == cur_lc)
        preferred_blocked.store(cur, cpp::MemoryOrder::RELEASE);
    }

    state->in_signal_handler = true;
  }

  ~HandlerScope() {
    if (!state)
      return;

    // Pop restart state.
    signal_restart::pop(state->restart);

    // Clear alt-stack flag if we set it.
    if (alt_stack_set)
      state->alt_stack_flags &= ~SS_ONSTACK;

    state->in_signal_handler = false;
    state->handler_ran = true;

    // Restore signal mask.
    if (mask_saved) {
      state->blocked_signals = old_mask;
      auto *restore_lc = get_current_lifecycle();
      if (restore_lc &&
          preferred_thread.load(cpp::MemoryOrder::RELAXED) == restore_lc)
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
    state->alt_stack_flags |= SS_ONSTACK;
    scope.alt_stack_set = true;
    auto *top = reinterpret_cast<void *>(
        (reinterpret_cast<ULONG_PTR>(state->alt_stack_sp) +
         state->alt_stack_size) &
        ~ULONG_PTR{0xF});
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

// Build a default siginfo_t for a standard signal.
void build_standard_siginfo(siginfo_t *info, int signum) {
  __builtin_memset(info, 0, sizeof(siginfo_t));
  info->si_signo = signum;
  info->si_code = SI_USER;
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch state transitions
// ---------------------------------------------------------------------------

namespace signal_dispatch {

void trigger(ThreadSignalState *state) {
  if (!state)
    return;

  // CAS IDLE → TRIGGERED. If already TRIGGERED or DRAINING, the pending
  // bit is set — the drain loop's re-check will see it.
  uint8_t expected = DISPATCH_STATE_IDLE;
  state->dispatch_state.compare_exchange_strong(
      expected, DISPATCH_STATE_TRIGGERED, cpp::MemoryOrder::RELEASE,
      cpp::MemoryOrder::RELAXED);
}

// APC callback for process-directed signal dispatch. Queued to a target
// thread to force dispatch even when the thread is busy-spinning and not
// at a cooperative dispatch boundary. The special user APC fires at the
// next kernel-to-user transition (timer interrupt, ~15ms), analogous to
// how Linux delivers signals at the return-to-userspace boundary.
static void NTAPI dispatch_apc_callback(PVOID /*arg1*/, PVOID /*arg2*/,
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
  // Fast path: try the preferred thread.
  DWORD alert_tid = 0;
  {
    EpochGuard guard;
    ThreadLifecycle *preferred =
        g_pcb.signal_dispatch.preferred_thread.load(
            cpp::MemoryOrder::ACQUIRE);
    if (preferred &&
        preferred->owner_tid.load(cpp::MemoryOrder::ACQUIRE) != 0 &&
        preferred->signal) {
      trigger(preferred->signal);
      HANDLE h = registry_borrow_handle(preferred);
      if (h)
        queue_dispatch_apc_to(h);
      alert_tid = preferred->tid;
    }
  }
  // Release epoch pin before the alert — alert_thread uses a TID (value
  // type), not a borrowed pointer. Matches cancel_support.cpp discipline.
  if (alert_tid) {
    alert_thread(alert_tid);
    return;
  }

  // Slow path: find the first thread with signal state and trigger it.
  // registry_for_each auto-pins; the visitor runs under the epoch guard.
  registry_for_each([&](ThreadLifecycle *target) -> bool {
    if (!target->signal)
      return false;
    trigger(target->signal);
    HANDLE h = registry_borrow_handle(target);
    if (h)
      queue_dispatch_apc_to(h);
    alert_tid = target->owner_tid.load(cpp::MemoryOrder::RELAXED);
    return true; // stop after first match
  }, 0);
  if (alert_tid)
    alert_thread(alert_tid);
}

// ---------------------------------------------------------------------------
// Main dispatch loop
// ---------------------------------------------------------------------------

void dispatch_pending(ThreadSignalState *state) {
  if (!state)
    return;

  // Check stack space before entering dispatch.
  if (!signal_stack::can_dispatch(state))
    return;

  // Claim the dispatch. Exchange to DRAINING.
  // If already DRAINING, another dispatch is in progress — return.
  uint8_t prev =
      state->dispatch_state.exchange(DISPATCH_STATE_DRAINING,
                                     cpp::MemoryOrder::ACQ_REL);
  if (prev == DISPATCH_STATE_DRAINING)
    return; // Reentrant — outer dispatch handles it.

  // Recover from siglongjmp: if a previous handler exited via longjmp,
  // the restart push/pop, in_signal_handler flag, and alt-stack flag may
  // be stale. dispatch_pending is the only call site for invoke_handler_impl,
  // and DRAINING prevents reentrancy, so depth must be 0 at entry. If not,
  // a handler longjmp'd — reset to clean state. POSIX siglongjmp restores
  // the signal mask independently, so only these fields need recovery.
  if (state->restart.depth != 0)
    state->restart = {};
  state->in_signal_handler = false;
  state->alt_stack_flags &= ~SS_ONSTACK;

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

  // Outer loop: drain all pending signals, re-check, retry on CAS failure.
  // The loop exits only when:
  //   (a) nothing pending and CAS DRAINING→IDLE succeeds, or
  //   (b) stack exhausted.
  bool drain_complete = false;
  while (!drain_complete && signal_stack::can_dispatch(state)) {
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
      if (!signal_stack::can_dispatch(state))
        goto done;

      // Try thread-local first (cheaper — single consumer, no contention).
      int sig = signal_pending::drain_next_standard(state->pending,
                                                     blocked_bits);
      // Then try process-wide (CAS contention possible but rare).
      if (sig == 0)
        sig = signal_pending::drain_next_standard(dispatch.process_pending,
                                                   blocked_bits);
      if (sig == 0)
        break;

      struct sigaction action = handler_table::read_and_consume(sig);
      siginfo_t info;
      build_standard_siginfo(&info, sig);
      if (!invoke_handler_impl(state, sig, &info, &action)) {
        // Nesting overflow — re-pend so the signal is retried after an
        // outer handler returns and frees a restart stack slot.
        signal_pending::pend_standard(state->pending, sig);
        break; // Stop draining — can't invoke any more handlers.
      }

      // Re-read blocked mask — handler may have changed it.
      blocked_bits = sigset_to_bits(state->blocked_signals);
      rt_mask = blocked_to_rt_mask(blocked_bits);
    }

    // ---- Phase 3: Deliver RT signals (lowest-numbered first, FIFO within) ----
    for (;;) {
      if (!signal_stack::can_dispatch(state))
        goto done;

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

    // ---- Phase 4: Re-check ----
    // Explicit re-check protocol. New signals may have arrived
    // during handler execution (APC, VEH). Check both thread-local and
    // process-wide pending sets, plus the inboxes.
    //
    // For standard signals, the bitmap bit is authoritative (cleared by
    // drain_next_standard). For RT signals, the bitmap bit is a presence
    // hint that may become stale after delivery — the inbox + buckets are
    // the source of truth. So we check standard bits via the bitmap, and
    // RT signals via inbox emptiness + bucket state. This prevents infinite
    // looping on stale RT presence bits.
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
        buckets.has_deliverable(blocked_to_rt_mask(recheck_blocked)))
      continue; // New signals — loop back.

    // Nothing pending. Attempt CAS DRAINING → IDLE.
    // If a transport set TRIGGERED between our re-check and
    // the CAS, the CAS fails — we loop back to drain the new signal.
    uint8_t expected = DISPATCH_STATE_DRAINING;
    if (state->dispatch_state.compare_exchange_strong(
            expected, DISPATCH_STATE_IDLE, cpp::MemoryOrder::RELEASE,
            cpp::MemoryOrder::ACQUIRE)) {
      drain_complete = true; // Exit the while loop.
    }
    // CAS failed → expected was set to TRIGGERED by a transport.
    // Loop back: the while condition re-checks can_dispatch and we
    // re-enter the drain phases to handle the new signal.
  }

done:
  // Re-push any RT entries that were absorbed from the inbox into buckets
  // but could not be delivered (because they were blocked). Without this,
  // the entries are leaked — removed from the inbox but never returned —
  // leaving the bitmap bit set with no inbox entry. The next dispatch_pending
  // would loop forever: absorb drains an empty inbox, buckets are empty,
  // but the stale bitmap bit keeps the re-check loop spinning.
  if (buckets.bitmap)
    buckets.repush_remaining(state->pending);

  // If we exited due to stack exhaustion (not drain_complete), force IDLE.
  if (!drain_complete)
    state->dispatch_state.store(DISPATCH_STATE_IDLE,
                                cpp::MemoryOrder::RELEASE);

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
