//===-- Windows signal state lifecycle management -----------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread state lifecycle: per-thread state init/cleanup, thread registry,
// and the global signal state singleton.
//
// The signal subsystem does NOT own a TEB TLS slot. Per-thread state is
// accessed via get_current_lifecycle()->signal. The lifecycle TLS root
// (thread_lifecycle.cpp) owns the single TEB slot and drives cleanup.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/CPP/new.h"
#include "src/__support/OSUtil/windows/io.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/OSUtil/windows/process/spawn_runtime_data.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/signal/control/process_control.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_engine.h"
#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/OSUtil/windows/signal/transport/alpc_transport.h"
#include "src/__support/OSUtil/windows/signal/transport/console_transport.h"
#include "src/__support/OSUtil/windows/signal/transport/veh_transport.h"
#include "src/__support/threads/windows/robust_list_cleanup.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Defined in longjmp.cpp with selectany — resolve the weak reference so
// longjmp can clear SS_ONSTACK without pulling in the signal subsystem.
extern "C" ThreadSignalState *(*get_thread_state_noinit_ptr)();

// Main thread state — canonical PCB-resident objects. The PCB holds
// the storage as opaque bytes (see MainThreadState in
// process_control_block.h for the why); these accessors reinterpret
// the bytes as the typed objects. Memory is zero-initialized by the
// PE loader together with the rest of the PCB; init_signal_state
// runs the typed zero-initializers (zero_thread_state /
// zero_lifecycle) over the storage before any consumer touches it.
static ThreadSignalState &main_thread_state() {
  return *reinterpret_cast<ThreadSignalState *>(
      g_pcb.main_thread.signal_storage);
}

static ThreadLifecycle &main_thread_lifecycle() {
  return *reinterpret_cast<ThreadLifecycle *>(
      g_pcb.main_thread.lifecycle_storage);
}

// Inherited child SectionRegion — file-local storage. The PCB stores a
// void* pointer to this (g_pcb.signal_child.inherited_region). Only processes
// spawned by NTPOSIX parents ever construct into this storage.
alignas(windows::SectionRegion) static unsigned char
    inherited_region_storage[sizeof(windows::SectionRegion)] = {};

static windows::SectionRegion &inherited_region() {
  return *reinterpret_cast<windows::SectionRegion *>(inherited_region_storage);
}

static void zero_thread_state(ThreadSignalState *state) {
  for (unsigned i = 0; i < __NSIGSET_WORDS; ++i)
    state->blocked_signals.__signals[i] = 0;
  state->pending.init();
  state->exit_gate.store(0, cpp::MemoryOrder::RELAXED);
  state->waiting_signals.store(0, cpp::MemoryOrder::RELAXED);
  state->last_fault_pc = nullptr;
  state->last_fault_code = 0;
  state->alt_stack_sp = nullptr;
  state->alt_stack_size = 0;
  state->alt_stack_flags = SS_DISABLE;
  state->owned_state = false;
  state->stop_parked.store(false, cpp::MemoryOrder::RELAXED);
  state->hard_suspended.store(false, cpp::MemoryOrder::RELAXED);
  state->sched_policy.store(0, cpp::MemoryOrder::RELAXED); // SCHED_OTHER
  state->dispatching = false;
  state->notify_word = nullptr;
  state->restart = {};
  state->interrupted_context = nullptr;
  state->refault_reset_signum = 0;
  state->in_signal_handler = false;
  state->handler_ran = false;
  state->autodisarm_saved_sp = nullptr;
  state->autodisarm_saved_size = 0;
  state->autodisarm_saved_flags = 0;
}

// Called from lifecycle_cleanup (thread_lifecycle.cpp) on thread exit.
// This is the signal subsystem's cleanup entry point — NOT a TLS callback.
//
// Signal transfer on exit (see signal drop audit 2026-04-20):
//   Cross-thread senders (pthread_kill, pthread_sigqueue) pend into
//   state->pending before queuing a notification APC. If the kernel drops
//   the APC during thread termination, the pended signal must still reach
//   a surviving thread — we transfer state->pending to process_pending.
//
//   The exit gate (ThreadSignalState::exit_gate) makes this drain race-free
//   for standard AND RT signals without any lock:
//     1. Set DRAINING: new senders redirect to process_pending at entry.
//     2. Spin until in-flight count is zero: existing senders complete
//        their pend and release the gate.
//     3. Drain: now genuinely single-consumer — no concurrent push can
//        occur. RT MPSC try_pop is trivially safe, and the standard-bit
//        exchange captures everything pended before we arrived.
//     4. Clear owner_tid via registry_deregister: late senders who observe
//        zero through the usual pthread_kill path take the ESRCH fast path
//        at apc_transport, but need no rescue — the gate guarantees all
//        successful pends were drained.
void deregister_thread_state(ThreadSignalState *state) {
  if (!state)
    return;
  auto *lc = get_current_lifecycle();
  if (lc) {
    // If we are currently the cached dispatcher, evict ourselves so
    // future signal-arrival paths re-resolve through the registry.
    uint64_t self_packed = ThreadHandle{lc->tid, lc->task_id}.pack();
    uint64_t cur =
        g_pcb.signal_dispatch.preferred_thread.load(cpp::MemoryOrder::ACQUIRE);
    if (cur == self_packed) {
      g_pcb.signal_dispatch.preferred_thread.compare_exchange_strong(
          cur, 0, cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::RELAXED);
    }

    // Close the gate. fetch_or returns the value just before the OR so we
    // can read the in-flight count in the same atomic step.
    uint32_t prev = state->exit_gate.fetch_or(
        ThreadSignalState::EXIT_GATE_DRAINING, cpp::MemoryOrder::ACQ_REL);

    // Wait for existing in-flight senders to release. New senders arriving
    // from this point see DRAINING and bail without pending — they don't
    // contribute to the count. Bounded by sender-side critical section,
    // which is a single pend_into call (~sub-µs in all paths).
    // Address-monitor park on the exit_gate cache line — wakes on the
    // sender's fetch_sub without burning cycles. DRAINING bit is stable
    // from this point (only we toggle it), so any observed word change
    // means the in-flight counter moved; reload and re-check the mask.
    while ((prev & ~ThreadSignalState::EXIT_GATE_DRAINING) != 0) {
      spin_wait::spin_on_raw(
          reinterpret_cast<uint32_t *>(&state->exit_gate.val), prev, 4096);
      prev = state->exit_gate.load(cpp::MemoryOrder::ACQUIRE);
    }

    // Snapshot whether we actually had any pending signals to drain.
    // No senders → empty pending → nothing to transfer, nobody to wake.
    // Skipping the trigger_any_thread call in the empty case avoids
    // injecting a special user APC into a peer thread that may be parked
    // in a non-alertable RtlWaitOnAddress for unrelated work — those APCs
    // would interrupt the wait and perturb timing-sensitive code paths
    // without delivering any actual signal.
    bool had_pending = state->pending.standard.load(
                           cpp::MemoryOrder::ACQUIRE) != 0;

    // Now the sole consumer of state->pending. Drain to process_pending.
    // No retry, no fence, no second pass — the gate closed the race.
    signal_pending::transfer_all(state->pending,
                                 g_pcb.signal_dispatch.process_pending);

    registry_deregister(lc); // Clears owner_tid (RELEASE).

    // Wake a thread to dispatch the transferred signals only if there
    // were any — see the snapshot above for the rationale.
    if (had_pending)
      signal_dispatch::trigger_any_thread();
  }
}

void init_signal_state() {
  // All process-wide signal fields are zero-initialized by the PE loader.
  // Zero-init gives: handlers = SIG_DFL, locks = unlocked, bitmasks = 0,
  // flags = false. Only non-zero init and subsystem setup needed here.
  g_pcb.signal_transport.veh_registered.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.console_registered.store(0,
                                                  cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.tty_winsize.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.tty_winsize_initialized.store(
      0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.tty_hangup_sent.store(0,
                                               cpp::MemoryOrder::RELAXED);
  g_pcb.signal_handler.ignored.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_handler.custom.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_dispatch.process_pending.init();

  // Initialize allocator pools. ThreadSignalState is now inlined in
  // ThreadLifecycle (lc->sig_state), so no separate signal-state pool
  // exists — only the RT sigqueue pool remains here.
  sigqueue_pool_init();

  // parent_pid is in Zone 0, already set by pcb_startup_init() (Phase 0).
  // No NtQueryInformationProcess needed here.

  // Handlers: zero-init = SIG_DFL. No loop needed.
  // Explicit zero for safety during fork reinit (called from signal_fork_reinit
  // which may have stale handler state from the parent).
  for (int i = 0; i < NSIG; ++i) {
    g_pcb.signal_handler.handlers[i].sa_handler = SIG_DFL;
    g_pcb.signal_handler.handlers[i].sa_flags = 0;
    for (unsigned j = 0; j < __NSIGSET_WORDS; ++j)
      g_pcb.signal_handler.handlers[i].sa_mask.__signals[j] = 0;
  }

  // Pre-register the main thread's state (PCB-resident, no heap).
  // The PCB also holds a separate `MainThreadState::signal` block; the
  // main lifecycle uses the inline `lc->sig_state` for signal storage,
  // and `g_pcb.main_thread.signal` is left as the canonical accessor
  // alias by routing `lc->signal` at it. Other lifecycles point
  // `signal` at `&this->sig_state`.
  auto &main_lc = main_thread_lifecycle();
  zero_lifecycle(&main_lc);
  main_lc.task_id = allocate_task_id();
  main_lc.tid = NtCurrentThreadId();
  main_lc.signal.store(&main_thread_state(), cpp::MemoryOrder::RELAXED);

  // Initialize the notification word on the main thread. Sets owner_tid_
  // from NtCurrentThreadId() for cross-thread alert delivery.
  main_lc.notify_word.init();

  // Set the lifecycle as the TEB root (lifecycle_startup_init() ran in Phase 4).
  set_current_lifecycle(&main_lc);

  zero_thread_state(&main_thread_state());
  // Wire the signal state's notify_word back-pointer to the lifecycle.
  main_thread_state().notify_word = &main_lc.notify_word;
  registry_register_self(&main_lc);

  // Set preferred thread for process-directed signal delivery. The
  // cache stores a packed ThreadHandle (re-resolved on every load) —
  // a raw lifecycle pointer wouldn't be safe across Crystalline retire.
  g_pcb.signal_dispatch.preferred_thread.store(
      ThreadHandle{main_lc.tid, main_lc.task_id}.pack(),
      cpp::MemoryOrder::RELEASE);

  // Register with longjmp's weak reference so SS_ONSTACK cleanup works.
  get_thread_state_noinit_ptr = get_thread_state_noinit;
}

ThreadSignalState *get_thread_state_noinit() {
  auto *lc = get_current_lifecycle();
  return lc ? lc->signal.load(cpp::MemoryOrder::ACQUIRE) : nullptr;
}

ThreadSignalState *get_thread_state() {
  auto *lc = get_current_lifecycle();
  if (lc) {
    if (auto *sig = lc->signal.load(cpp::MemoryOrder::ACQUIRE))
      return sig;
  }

  // Foreign thread touching signal APIs for the first time. The
  // lifecycle's signal state is inline (lc->sig_state); allocating a
  // lifecycle is the only allocation needed.
  if (!lc) {
    lc = alloc_lifecycle();
    if (!lc)
      return nullptr;
    lc->task_id = allocate_task_id();
    lc->tid = NtCurrentThreadId();
    // alloc_lifecycle wires lc->signal = &lc->sig_state for us.
    // Initialize notify_word for this foreign thread.
    lc->notify_word.init();
    set_current_lifecycle(lc);
  }

  ThreadSignalState *state = lc->signal.load(cpp::MemoryOrder::ACQUIRE);
  zero_thread_state(state);
  state->owned_state = true;
  state->notify_word = &lc->notify_word;

  if (!registry_register_self(lc)) {
    // Registration failed (OOM in slab). Roll back to avoid returning
    // a state that looks valid but is invisible to cancel/kill/fork.
    set_current_lifecycle(nullptr);
    free_lifecycle(lc);
    return nullptr;
  }

  return state;
}

void register_thread_state(ThreadSignalState *state) {
  zero_thread_state(state);

  // The lifecycle is already set in TEB by the thread creation path.
  auto *lc = get_current_lifecycle();
  if (lc) {
    // Initialize the notification word for this thread. Must be called on
    // the owning thread — sets owner_tid_ from NtCurrentThreadId().
    lc->notify_word.init();

    // RELEASE: cross-thread senders ACQUIRE-load `signal` and must
    // observe `state`'s subsequent zero-init (already done above) plus
    // the notify_word back-pointer set below.
    lc->signal.store(state, cpp::MemoryOrder::RELEASE);
    // Wire the back-pointer so trigger() can reach notify_word from state.
    state->notify_word = &lc->notify_word;
    // Only self-register if not already registered. The parent thread
    // pre-registers the child in Thread::run() to avoid a window where
    // pthread_cancel returns ESRCH. registry_register is idempotent for
    // (lc, tid) — the second call is a no-op (returns true).
    registry_register_self(lc);
  }

  // If a stop is in progress, set the STOP bit and park immediately.
  // signal_or on our own notify_word is safe — the owner (us) will see
  // the bit on the next test_flags() inside check_stop_request().
  if (g_pcb.signal_stop.phase.load(cpp::MemoryOrder::ACQUIRE) !=
      process_control::PHASE_RUNNING) {
    if (lc)
      ThreadLocalWord::signal_or(&lc->notify_word, notify::STOP);
    process_control::check_stop_request(state);
  }
}

void fini_signal_state() {
  // Run cleanup for the main thread. Other threads are cleaned up by
  // their lifecycle_cleanup callback on thread exit.
  auto *lc = get_current_lifecycle();
  if (lc) {
    registry_deregister(lc);
    lc->signal.store(nullptr, cpp::MemoryOrder::RELEASE);
  }

  // Clear the packed-handle dispatcher cache.
  g_pcb.signal_dispatch.preferred_thread.store(0, cpp::MemoryOrder::RELAXED);
}

void init_inherited_child_state() {
  internal::process_utils::InheritedRuntimeDataView inherited = {};
  if (!internal::process_utils::get_inherited_runtime_data(&inherited))
    return;

  DWORD count = *reinterpret_cast<const DWORD *>(inherited.data);
  SIZE_T standard_size = sizeof(DWORD) + count * (1 + sizeof(HANDLE));
  SIZE_T aligned_offset = (standard_size + 7) & ~SIZE_T{7};

  if (inherited.size < aligned_offset + RESERVED2_EXT_SIZE)
    return;

  auto *r2 = reinterpret_cast<const Reserved2Ext *>(inherited.data +
                                                    aligned_offset);
  if (r2->magic != LLVM_LIBC_RESERVED2_MAGIC)
    return;

  g_pcb.signal_child.inherited_event = r2->event;

  // Adopt the inherited section handle and map it through a placeholder.
  // SectionRegion::map_existing takes ownership of the handle on success.
  // Construct into file-local storage; PCB holds a void* pointer.
  ::new (inherited_region_storage) windows::SectionRegion(
      windows::SectionRegion::map_existing(
          r2->section, sizeof(ChildStateBlock), PAGE_READWRITE));
  if (!inherited_region())
    return;
  g_pcb.signal_child.inherited_region = &inherited_region();

  // Mark that this child has already exec'd (posix_spawn is atomic
  // fork+exec). The parent reads exec_count to enforce POSIX setpgid
  // semantics (EACCES after exec). For future fork children, exec_count
  // would start at 0 and be incremented in the execve engine.
  auto *block = inherited_region().as<ChildStateBlock>();
  if (block)
    block->exec_count.store(1, cpp::MemoryOrder::RELEASE);

  // POSIX_SPAWN_SETSID: child becomes session leader.
  // setsid() = session ID = own PID, pgid = own PID, detach console.
  if (r2->attr_flags & SPAWN_ATTR_SETSID) {
    pid_t self = static_cast<pid_t>(NtCurrentProcessId());
    // Set session ID to our PID.
    windows_syscalls::g_session_id.store(self, cpp::MemoryOrder::RELAXED);
    windows_syscalls::g_session_id_initialized.store(true,
                                                     cpp::MemoryOrder::RELEASE);
    // Become process group leader.
    auto *params = NtCurrentPeb()->ProcessParameters;
    if (params)
      params->ProcessGroupId = static_cast<ULONG>(self);
    process::set_self_pgid(self);
    // Detach from controlling terminal.
    (void)console::disconnect();
  }

  if (r2->attr_flags & SPAWN_ATTR_SETSIGMASK) {
    bits_to_sigset(r2->sigmask, &main_thread_state().blocked_signals);
  }

  if (r2->attr_flags & SPAWN_ATTR_SETSIGDEF) {
    uint64_t sigdef = r2->sigdefault;
    if (sigdef & (1ULL << (2 - 1))) // SIGINT = 2
      console::set_ctrl_c_ignore(false);
  }
}

void fini_inherited_child_state() {
  // SectionRegion::destroy() unmaps the view and closes the section handle.
  if (g_pcb.signal_child.inherited_region) {
    inherited_region().destroy();
    g_pcb.signal_child.inherited_region = nullptr;
  }
  if (g_pcb.signal_child.inherited_event) {
    ::NtClose(g_pcb.signal_child.inherited_event);
    g_pcb.signal_child.inherited_event = nullptr;
  }
}

int signal_subsystem_init() {
  init_signal_state();

  // Initialize cross-process ALPC transport: create private namespace,
  // create port, build security descriptor, start listener thread.
  // Non-fatal: if ALPC init fails, cross-process signal delivery is
  // disabled but intra-process operations still work.
  alpc_transport::init();

  return 0;
}

void signal_subsystem_fini() {
  // Shutdown ALPC transport first — stop listener thread, close port,
  // clear connection cache, close namespace.
  alpc_transport::fini();

  console_transport::remove();
  // VEH transport is statically registered via .libcveh — no explicit
  // removal. The static record stays for the life of the image; the
  // handler's runtime gate (custom_mask) is cleared as user handlers
  // are torn down.
  fini_inherited_child_state();
  fini_signal_state();
}

// Fork child reinit: reset locks, rebuild per-process kernel state, clean
// thread registry to just the forking thread.
void signal_fork_reinit() {
  g_pcb.signal_handler.lock.reset_for_fork();

  ThreadLifecycle *self_lc = get_current_lifecycle();
  registry_fork_reinit(self_lc);

  if (self_lc) {
    g_pcb.signal_dispatch.preferred_thread.store(
        ThreadHandle{self_lc->tid, self_lc->task_id}.pack(),
        cpp::MemoryOrder::RELEASE);
    ThreadSignalState *self_sig =
        self_lc->signal.load(cpp::MemoryOrder::RELAXED);
    if (self_sig) {
      g_pcb.signal_dispatch.preferred_blocked.store(
          sigset_to_bits(self_sig->blocked_signals),
          cpp::MemoryOrder::RELEASE);
      self_sig->dispatching = false;
      self_sig->stop_parked.store(false, cpp::MemoryOrder::RELAXED);
      self_sig->hard_suspended.store(false, cpp::MemoryOrder::RELAXED);

      // POSIX: the child's pending signal set is empty after fork(). Clear
      // the surviving thread's per-thread pending bitmap/RT queue as well as
      // any stale sigsuspend/sigtimedwait wait mask inherited COW from the
      // parent — the child thread is not in sigwait at fork exit.
      signal_pending::clear_all(self_sig->pending);
      self_sig->waiting_signals.store(0, cpp::MemoryOrder::RELAXED);
    }
    self_lc->active_syscall.store(nullptr, cpp::MemoryOrder::RELAXED);
  }

  // The parent's process-state-change handle was created with internal_oa()
  // (non-inheritable), so it doesn't exist in the child's handle table.
  // Just null the stale pointer — no NtClose.
  g_pcb.signal_child.process_state_change = nullptr;

  // Reset process-wide pending signals.
  signal_pending::clear_all(g_pcb.signal_dispatch.process_pending);

  // Reset stop state. The reactor death watch (if any) was created by the
  // parent — the WCP handle is non-inheritable, so it doesn't exist in the
  // child. Just null the fields.
  g_pcb.signal_stop.phase.store(process_control::PHASE_RUNNING,
                                cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.coordinator_tid.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.ack_gen.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.target_count.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.coordinator_handle = nullptr;
  g_pcb.signal_stop.death_watch = internal::reactor::INVALID_TOKEN;
  g_pcb.signal_stop.death_watch_registered.store(false,
                                                  cpp::MemoryOrder::RELAXED);

  // Reinitialize ALPC transport for the child: close inherited parent port,
  // create child's own port with child's PID+create_time, start new
  // listener, clear stale connection cache.
  alpc_transport::fork_reinit();

  // Reset sigqueue pool for the child.
  sigqueue_pool_fork_reinit();

  g_pcb.signal_transport.veh_registered.store(0,
                                              cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.console_registered.store(0,
                                                  cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.tty_winsize.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.tty_winsize_initialized.store(
      0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_transport.tty_hangup_sent.store(0,
                                               cpp::MemoryOrder::RELAXED);

  // parent_pid is in Zone 0b — updated by pcb_unseal_readonly_b() at the start of libc_fork_reinit().

  fini_inherited_child_state();
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::signal_startup_init() {
  LIBC_NAMESPACE::signal_state::signal_subsystem_init();
  return 0;
}

void LIBC_NAMESPACE::internal::signal_fork_reinit() {
  LIBC_NAMESPACE::signal_state::signal_fork_reinit();
}

// ---------------------------------------------------------------------------
// Exec handler reset — POSIX exec semantics:
//   - Caught handlers → SIG_DFL
//   - SIG_IGN preserved
//   - SIG_DFL preserved
//   - Process-pending signals cleared (implementation-defined, we clear)
//   - Alternate signal stack not preserved (POSIX)
//   - Signal mask preserved (POSIX: "unchanged")
//
// Called from exec Phase 5 after quiesce_threads. Single-threaded at this
// point — no lock contention.
// ---------------------------------------------------------------------------
void LIBC_NAMESPACE::internal::signal_exec_reset_handlers() {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::signal_state;

  auto &handler = g_pcb.signal_handler;

  // Single-threaded after quiesce — lock for consistency with any
  // in-flight dispatch that might have been frozen mid-read.
  handler.lock.lock();

  for (int sig = 1; sig < NSIG; ++sig) {
    auto *h = &handler.handlers[sig];
    // POSIX: caught handlers reset to SIG_DFL. SIG_IGN and SIG_DFL kept.
    if (h->sa_handler != SIG_IGN && h->sa_handler != SIG_DFL) {
      h->sa_handler = SIG_DFL;
      h->sa_flags = 0;
      __builtin_memset(&h->sa_mask, 0, sizeof(h->sa_mask));
    }
  }

  // Batch-update bitmasks: no custom handlers remain after exec.
  // ignored stays as-is (SIG_IGN is preserved across exec).
  handler.custom.store(0, cpp::MemoryOrder::RELEASE);

  handler.lock.unlock();

  // Clear process-pending signals. POSIX: "it is unspecified whether
  // pending signals are discarded" — we choose to clear for clean state.
  signal_state::signal_pending::clear_all(
      g_pcb.signal_dispatch.process_pending);

  // Clear thread-pending for the surviving (current) thread.
  auto *tss = signal_state::get_thread_state_noinit();
  if (tss) {
    signal_state::signal_pending::clear_all(tss->pending);

    // POSIX: "Alternate signal stacks are not preserved across exec."
    tss->alt_stack_sp = nullptr;
    tss->alt_stack_size = 0;
  }
}

LIBC_REGISTER_FINI(8, signal,
                   &::LIBC_NAMESPACE::signal_state::signal_subsystem_fini)

LIBC_REGISTER_FORK_REINIT(signal,
                          ::LIBC_NAMESPACE::internal::kForkPrioSignal,
                          &::LIBC_NAMESPACE::internal::signal_fork_reinit)
