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
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Defined in longjmp.cpp with selectany — resolve the weak reference so
// longjmp can clear SS_ONSTACK without pulling in the signal subsystem.
extern "C" ThreadSignalState *(*get_thread_state_noinit_ptr)();

// Main thread state — canonical PCB-resident objects, zero-initialized by the
// PE loader with the rest of the PCB.
static ThreadSignalState &main_thread_state() {
  return g_pcb.main_thread.signal;
}

static ThreadLifecycle &main_thread_lifecycle() {
  return g_pcb.main_thread.lifecycle;
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
  state->waiting_signals.store(0, cpp::MemoryOrder::RELAXED);
  state->last_fault_pc = nullptr;
  state->last_fault_code = 0;
  state->alt_stack_sp = nullptr;
  state->alt_stack_size = 0;
  state->alt_stack_flags = SS_DISABLE;
  state->owned_state = false;
  state->stop_state.store(process_control::STOP_RUNNING,
                          cpp::MemoryOrder::RELAXED);
  state->sched_policy.store(0, cpp::MemoryOrder::RELAXED); // SCHED_OTHER
  state->dispatch_state.store(DISPATCH_STATE_IDLE, cpp::MemoryOrder::RELAXED);
  state->restart = {};
  state->interrupted_context = nullptr;
  state->refault_reset_signum = 0;
  state->in_signal_handler = false;
  state->handler_ran = false;
}

// Called from lifecycle_cleanup (thread_lifecycle.cpp) on thread exit.
// This is the signal subsystem's cleanup entry point — NOT a TLS callback.
void deregister_thread_state(ThreadSignalState *state) {
  if (!state)
    return;
  auto *lc = get_current_lifecycle();
  if (lc) {
    ThreadLifecycle *preferred =
        g_pcb.signal_dispatch.preferred_thread.load(cpp::MemoryOrder::ACQUIRE);
    if (preferred == lc) {
      g_pcb.signal_dispatch.preferred_thread.compare_exchange_strong(
          preferred, nullptr, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED);
    }
    registry_deregister(lc);
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

  // Initialize the sigqueue entry pool.
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
  auto &main_lc = main_thread_lifecycle();
  zero_lifecycle(&main_lc);
  main_lc.task_id = allocate_task_id();
  main_lc.tid = static_cast<int>(NtCurrentThreadId());
  main_lc.owner_tid.store(NtCurrentThreadId(), cpp::MemoryOrder::RELAXED);
  main_lc.signal = &main_thread_state();

  // Set the lifecycle as the TEB root (lifecycle_startup_init() ran in Phase 4).
  set_current_lifecycle(&main_lc);

  zero_thread_state(&main_thread_state());
  registry_register_self(&main_lc);

  // Set preferred thread for process-directed signal delivery.
  g_pcb.signal_dispatch.preferred_thread.store(&main_lc,
                                               cpp::MemoryOrder::RELEASE);

  // Register with longjmp's weak reference so SS_ONSTACK cleanup works.
  get_thread_state_noinit_ptr = get_thread_state_noinit;
}

ThreadSignalState *get_thread_state_noinit() {
  auto *lc = get_current_lifecycle();
  return lc ? lc->signal : nullptr;
}

ThreadSignalState *get_thread_state() {
  auto *lc = get_current_lifecycle();
  if (lc && lc->signal)
    return lc->signal;

  // Foreign thread touching signal APIs for the first time. Allocate
  // both a lifecycle and a signal state.
  if (!lc) {
    lc = alloc_lifecycle();
    if (!lc)
      return nullptr;
    lc->task_id = allocate_task_id();
    lc->tid = static_cast<int>(NtCurrentThreadId());
    lc->pool_allocated = true;
    set_current_lifecycle(lc);
  }

  auto *state = pool_alloc();
  if (!state)
    return nullptr;

  zero_thread_state(state);
  state->owned_state = true;
  lc->signal = state;
  lc->signal_owned = true;

  SlotRef ref = registry_register_self(lc);
  if (!ref.is_valid()) {
    // Registration failed (OOM in slab). Roll back to avoid returning a
    // state that looks valid but is invisible to cancel/kill/fork.
    lc->signal = nullptr;
    lc->signal_owned = false;
    pool_free(state);
    if (lc->pool_allocated) {
      set_current_lifecycle(nullptr);
      free_lifecycle(lc);
    }
    return nullptr;
  }

  return state;
}

void register_thread_state(ThreadSignalState *state) {
  zero_thread_state(state);

  // The lifecycle is already set in TEB by the thread creation path.
  auto *lc = get_current_lifecycle();
  if (lc) {
    lc->signal = state;
    // Only self-register if not already registered. The parent thread
    // pre-registers the child in Thread::run() to avoid a window where
    // pthread_cancel returns ESRCH. Re-registering would orphan that slot
    // and leak the parent's handle.
    if (lc->slot_page.load(cpp::MemoryOrder::ACQUIRE) == UINT32_MAX)
      registry_register_self(lc);
  }

  // If a stop is in progress, park immediately.
  if (g_pcb.signal_stop.phase.load(cpp::MemoryOrder::ACQUIRE) !=
      process_control::PHASE_RUNNING) {
    state->stop_state.store(process_control::STOP_REQUESTED,
                            cpp::MemoryOrder::RELAXED);
    process_control::check_stop_request(state);
  }
}

void fini_signal_state() {
  // Run cleanup for the main thread. Other threads are cleaned up by
  // their lifecycle_cleanup callback on thread exit.
  auto *lc = get_current_lifecycle();
  if (lc) {
    registry_deregister(lc);
    lc->signal = nullptr;
  }

  // Clear preferred thread pointer.
  g_pcb.signal_dispatch.preferred_thread.store(nullptr,
                                               cpp::MemoryOrder::RELAXED);
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
  veh_transport::remove();
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
    g_pcb.signal_dispatch.preferred_thread.store(self_lc,
                                                 cpp::MemoryOrder::RELEASE);
    ThreadSignalState *self_sig = self_lc->signal;
    if (self_sig) {
      g_pcb.signal_dispatch.preferred_blocked.store(
          sigset_to_bits(self_sig->blocked_signals),
          cpp::MemoryOrder::RELEASE);
      self_sig->dispatch_state.store(DISPATCH_STATE_IDLE,
                                     cpp::MemoryOrder::RELAXED);
    }
    self_lc->active_syscall.store(nullptr, cpp::MemoryOrder::RELAXED);
  }

  // The parent's process-state-change handle was created with internal_oa()
  // (non-inheritable), so it doesn't exist in the child's handle table.
  // Just null the stale pointer — no NtClose.
  g_pcb.signal_child.process_state_change = nullptr;

  // Reset process-wide pending signals.
  signal_pending::clear_all(g_pcb.signal_dispatch.process_pending);

  // Reset stop state (all four fields — coordinator is packed TID|start).
  g_pcb.signal_stop.phase.store(process_control::PHASE_RUNNING,
                                cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.coordinator.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.ack_gen.store(0, cpp::MemoryOrder::RELAXED);
  g_pcb.signal_stop.target_count.store(0, cpp::MemoryOrder::RELAXED);

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

  // parent_pid is in Zone 0 — updated by pcb_unseal_readonly() at the start of libc_fork_reinit().

  fini_inherited_child_state();
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::signal_startup_init() {
  LIBC_NAMESPACE::signal_state::signal_subsystem_init();
  return 0;
}

void LIBC_NAMESPACE::internal::signal_startup_fini() {
  LIBC_NAMESPACE::signal_state::signal_subsystem_fini();
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
