//===-- Signal syscall engine functions ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal implementations of signal operations. All functions return long:
// 0 on success (or signal number for sigtimedwait), negated errno on failure.
//
// signal_state:: functions implement Linux syscall semantics in userspace.
// internal:: functions are engine functions for POSIX entry points.
//
// Uses the four-layer signal architecture:
//   Layer 1: pending_storage.h (PendingSet, pend/drain)
//   Layer 2: apc_transport.h (cross-thread/process delivery)
//   Layer 3: dispatch_engine.h (drain pending, invoke handlers)
//   Layer 4: process_control.h (stop/continue/default actions)
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/OSUtil/windows/signal/control/process_control.h"
#include "src/__support/OSUtil/windows/signal/control/sigchld.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_engine.h"
#include "src/__support/OSUtil/windows/signal/dispatch/handler_table.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/OSUtil/windows/signal/transport/alpc_transport.h"
#include "src/__support/OSUtil/windows/signal/transport/apc_transport.h"
#include "src/__support/OSUtil/windows/signal/transport/console_transport.h"
#include "src/__support/OSUtil/windows/signal/transport/veh_transport.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/process/windows/child_table.h"
#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/stack_t.h"
#include "hdr/types/struct_sigaction.h"
#include "hdr/types/struct_timespec.h"
#include "hdr/types/ucontext_t.h"
#include "hdr/types/uid_t.h"
#include "hdr/types/union_sigval.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

namespace {
// Get the real UID of the current process. Used to populate si_uid in
// siginfo_t for kill/sigqueue/pthread_kill (POSIX requirement).
uid_t get_current_uid() {
  return g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
}
} // namespace

// ===========================================================================
// signal_state:: bridge functions (public API from signal.h)
// ===========================================================================

namespace signal_state {

// ---------------------------------------------------------------------------
// Unified pend helper — routes standard vs RT signals correctly.
//
// Standard signals (1..SIGRTMIN-1) use the coalescing bitmap.
// RT signals (SIGRTMIN..SIGRTMAX) require an inbox entry (SigqueueEntry)
// allocated from the sigqueue pool. Without the entry, the bitmap bit is
// set but the dispatch engine's bucket absorb finds nothing, causing an
// infinite loop in dispatch_pending.
//
// Returns true on success, false on RT entry allocation failure.
// ---------------------------------------------------------------------------
static bool pend_signal(PendingSet &ps, int signum, int si_code) {
  if (signum >= SIGRTMIN) {
    SigqueueEntry *entry = sigqueue_alloc();
    if (!entry)
      return false;
    entry->si_signo = signum;
    entry->si_code = si_code;
    entry->value.sival_int = 0;
    entry->pid = static_cast<pid_t>(NtCurrentProcessId());
    entry->uid = 0;
    signal_pending::pend_rt(ps, entry);
  } else {
    (void)signal_pending::pend_standard(ps, signum, si_code);
  }
  return true;
}

bool is_signal_blocked(int signum) {
  if (!is_valid_signal(signum))
    return false;

  ThreadSignalState *state = get_thread_state();
  if (!state)
    return false;

  uint64_t mask = 1ULL << (signum - 1);
  return (sigset_to_bits(state->blocked_signals) & mask) != 0;
}

void generate_standard_signal_for_current_thread(int signum) {
  if (!is_valid_signal(signum))
    return;

  uint64_t bit = 1ULL << (signum - 1);

  // Lock-free: ignored dispositions are mirrored in an atomic bitmask.
  if (g_pcb.signal_handler.ignored.load(cpp::MemoryOrder::ACQUIRE) & bit)
    return;

  ThreadSignalState *state = get_thread_state();
  if (!state)
    return;

  // Pend to current thread regardless of blocked state.
  if (!pend_signal(state->pending, signum, SI_TKILL))
    return;
  signal_dispatch::trigger(state);

  // If unblocked, dispatch immediately.
  if (!(sigset_to_bits(state->blocked_signals) & bit))
    signal_dispatch::dispatch_pending(state);
}

bool should_restart_syscall() {
  ThreadSignalState *state = get_thread_state_noinit();
  if (!state)
    return false;

  return signal_dispatch::should_restart_syscall(state);
}

intptr_t deliver_signal_to_thread(DWORD tid, int signum) {
  // Self-delivery: synchronous, matching Linux kernel tgkill behavior.
  if (tid == NtCurrentThreadId()) {
    if (signum == 0)
      return 0;
    if (!is_valid_signal(signum))
      return -EINVAL;
    generate_standard_signal_for_current_thread(signum);
    return 0;
  }

  // Cross-thread: use APC transport (sender-side validation).
  //
  // si_code = SI_TKILL: this entry point implements tgkill/tkill/pthread_kill
  // semantics. Linux always reports SI_TKILL for thread-directed signals,
  // distinct from SI_USER (whole-process kill). Mirrors the self-target path
  // in generate_standard_signal_for_current_thread, which already pends with
  // SI_TKILL — same syscall must produce the same si_code regardless of
  // whether the target tid is self or a peer.
  //
  // si_pid = our own PID: required by Linux SI_TKILL siginfo contract
  // (sender PID). For intra-process tkill that's the calling process. The
  // APC transport's nullable-info default would otherwise leave si_pid = 0,
  // because info is non-null but value-initialised.
  siginfo_t info = {};
  info.si_signo = signum;
  info.si_code = SI_TKILL;
  info.si_pid = static_cast<pid_t>(NtCurrentProcessId());
  info.si_uid = get_current_uid();
  return apc_transport::send_to_thread(tid, signum, &info);
}

intptr_t deliver_process_signal(int signum) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  if (!pend_signal(g_pcb.signal_dispatch.process_pending, signum, SI_USER))
    return -EAGAIN; // RT signal queue allocation failed (system OOM).

  // Wake a thread to handle it.
  signal_dispatch::trigger_any_thread();
  return 0;
}

// SIGCHLD bridge — delegates to Layer 4.
void deliver_sigchld(int code, int pid, int status, long long utime_us,
                     long long stime_us) {
  process_control::deliver_sigchld(code, pid, status, utime_us, stime_us);
}

void populate_sigchld_info(siginfo_t *info) {
  process_control::populate_sigchld_info(info);
}

// Parent state change notification — delegates to Layer 4.
void notify_parent_state_change(int state_code) {
  process_control::notify_parent_state_change(state_code);
}

// ===========================================================================
// signal_state:: kernel functions
// ===========================================================================

// ---------------------------------------------------------------------------
// rt_sigaction helpers
// ---------------------------------------------------------------------------

// Signals delivered via console_transport::install().
static bool is_console_signal(int signum) {
  switch (signum) {
  case SIGINT:
  case SIGQUIT:
  case SIGHUP:
  case SIGTERM:
    return true;
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// Kernel function — implements SYS_rt_sigaction semantics.
// ---------------------------------------------------------------------------
intptr_t rt_sigaction(int signum, const void *act, void *oldact) {
  return rt_sigaction(signum, static_cast<const struct sigaction *>(act),
                      static_cast<struct sigaction *>(oldact));
}

intptr_t rt_sigaction(int signum, const struct sigaction *__restrict act,
                      struct sigaction *__restrict oldact) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  // SIGKILL and SIGSTOP cannot be caught or ignored.
  if (signum == SIGKILL || signum == SIGSTOP)
    return -EINVAL;

  if (oldact)
    *oldact = handler_table::read(signum);

  if (act) {
    // Use HandlerEntry for clean disposition classification.
    HandlerEntry entry;
    entry = *act;

    // Console transport is installed lazily on first console-class custom
    // handler. The VEH transport is statically registered into .libcveh at
    // link time; its handler self-gates on g_pcb.signal_handler.custom and
    // returns EXCEPTION_CONTINUE_SEARCH when no SEH-class handler is set.
    if (entry.disposition() == HandlerEntry::Disposition::Custom) {
      if (is_console_signal(signum))
        console_transport::install();
    }

    handler_table::write(signum, act);

    // Sync the OS-level Ctrl-C ignore flag with our userspace disposition.
    if (signum == SIGINT)
      console::set_ctrl_c_ignore(entry.is_ignored());
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Kernel function — implements SYS_rt_sigprocmask semantics.
// ---------------------------------------------------------------------------
intptr_t rt_sigprocmask(int how, const void *set, void *oldset) {
  return rt_sigprocmask(how, static_cast<const sigset_t *>(set),
                        static_cast<sigset_t *>(oldset));
}

intptr_t rt_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {

  ThreadSignalState *state = get_thread_state();
  if (!state)
    return -EAGAIN;

  // Return old mask if requested.
  if (oldset)
    *oldset = state->blocked_signals;

  // If set is null, just return old mask.
  if (!set)
    return 0;

  uint64_t new_mask = sigset_to_bits(*set);
  uint64_t current = sigset_to_bits(state->blocked_signals);

  switch (how) {
  case SIG_BLOCK:
    current |= new_mask;
    break;
  case SIG_UNBLOCK:
    current &= ~new_mask;
    break;
  case SIG_SETMASK:
    current = new_mask;
    break;
  default:
    return -EINVAL;
  }

  // SIGKILL and SIGSTOP cannot be blocked (POSIX).
  // Internal signals __SIGRTMIN through SIGRTMIN-1 (32-33) are reserved for
  // libc use and stripped from user masks, matching glibc NPTL behavior.
  current &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)) |
               (1ULL << (__SIGRTMIN - 1)) | (1ULL << (__SIGRTMIN)));

  bits_to_sigset(current, &state->blocked_signals);

  // Keep the console handler's blocked-mask snapshot in sync. The
  // dispatcher cache stores a packed ThreadHandle; if our handle
  // matches we own the dispatcher slot and can publish the new mask.
  auto &dispatch = g_pcb.signal_dispatch;
  ThreadHandle self_handle = current_thread_handle();
  if (self_handle.is_valid() &&
      dispatch.preferred_thread.load(cpp::MemoryOrder::RELAXED) ==
          self_handle.pack())
    dispatch.preferred_blocked.store(current, cpp::MemoryOrder::RELEASE);

  // Deliver any pending signals that are now unblocked.
  signal_dispatch::dispatch_pending(state);

  return 0;
}

// ---------------------------------------------------------------------------
// kill helpers
// ---------------------------------------------------------------------------

// Terminate a process by PID. Exit code follows the Unix convention:
// 128 + signum, so waitpid callers can detect signal-caused termination.
static int terminate_process(DWORD pid, int signum) {
  windows::ScopedNtHandle h;
  NTSTATUS st = ::NtOpenProcessById(h.put(), PROCESS_TERMINATE, pid);
  if (!NT_SUCCESS(st))
    return (st == STATUS_ACCESS_DENIED) ? EPERM : ESRCH;
  st = ::NtTerminateProcess(h.get(), 128 + signum);
  if (!NT_SUCCESS(st))
    return EPERM;
  return 0;
}

// Send a console control event.
static int send_console_ctrl(DWORD event, DWORD process_group_id) {
  NTSTATUS status = console::generate_ctrl_event(event, process_group_id);
  if (NT_SUCCESS(status))
    return 0;

  switch (status) {
  case STATUS_ACCESS_DENIED:
  case STATUS_PRIVILEGE_NOT_HELD:
    return EPERM;
  case STATUS_INVALID_CID:
  case STATUS_INVALID_HANDLE:
  case STATUS_INVALID_DEVICE_REQUEST:
  case STATUS_OBJECT_NAME_NOT_FOUND:
  case STATUS_OBJECT_PATH_NOT_FOUND:
    return ESRCH;
  default:
    return windows_util::ntstatus_to_errno(status);
  }
}

// Suspend all threads in a process (SIGSTOP) via state-change handle.
static int suspend_process(DWORD pid) {
  windows::ScopedNtHandle h;
  NTSTATUS st = ::NtOpenProcessById(h.put(), PROCESS_SUSPEND_RESUME, pid);
  if (!NT_SUCCESS(st))
    return (st == STATUS_ACCESS_DENIED) ? EPERM : ESRCH;
  windows::ScopedNtHandle sc;
  auto sc_oa = windows::internal_oa();
  st = ::NtCreateProcessStateChange(sc.put(), PROCESS_STATE_ALL_ACCESS, &sc_oa,
                                    h.get(), 0);
  if (!NT_SUCCESS(st))
    return EPERM;
  st = ::NtChangeProcessState(sc.get(), h.get(), ProcessStateSuspend, nullptr,
                              0, 0);
  if (!NT_SUCCESS(st))
    return EPERM;
  // Keep the state-change handle open so the kernel suspension persists after
  // this helper returns. SIGCONT uses NtResumeProcess rather than this handle.
  (void)sc.release();
  return 0;
}

// Resume all threads in a process (SIGCONT).
static int resume_process(DWORD pid) {
  windows::ScopedNtHandle h;
  NTSTATUS st = ::NtOpenProcessById(h.put(), PROCESS_SUSPEND_RESUME, pid);
  if (!NT_SUCCESS(st))
    return (st == STATUS_ACCESS_DENIED) ? EPERM : ESRCH;
  st = ::NtResumeProcess(h.get());
  return NT_SUCCESS(st) ? 0 : EPERM;
}

// Deliver a signal to another process using the best available mechanism.
static int deliver_signal_to_process(DWORD pid, int signum) {
  if (signum == 0) {
    // Existence check.
    windows::ScopedNtHandle h;
    NTSTATUS st =
        ::NtOpenProcessById(h.put(), PROCESS_QUERY_LIMITED_INFORMATION, pid);
    if (NT_SUCCESS(st))
      return 0;
    return (st == STATUS_ACCESS_DENIED) ? EPERM : ESRCH;
  }

  switch (signum) {
  case SIGKILL:
  case SIGTERM:
    return terminate_process(pid, signum);

  case SIGSTOP:
    return suspend_process(pid);

  case SIGCONT: {
    // NtResumeProcess handles kernel-level suspension.
    // ALPC delivery handles cooperative alert-park stop.
    int err = resume_process(pid);
    // Also deliver SIGCONT via ALPC for cooperative stop protocol.
    siginfo_t info = {};
    info.si_signo = SIGCONT;
    info.si_code = SI_USER;
    info.si_uid = get_current_uid();
    alpc_transport::send_to_process(pid, SIGCONT, &info);
    return err;
  }

  case SIGQUIT: {
    // CTRL_BREAK delivery is the preferred path for shells/terminals. It
    // only works when the target shares a console with us; children spawned
    // with SPAWN_ATTR_SETSID or started detached have no console, and
    // GenerateConsoleCtrlEvent returns STATUS_INVALID_HANDLE / ESRCH for
    // them. Fall back to ALPC so SIGQUIT still reaches the target. EPERM
    // is a real authorization failure — surface it unchanged.
    int err = send_console_ctrl(console::CTRL_BREAK_EVENT, pid);
    if (err == 0 || err == EPERM)
      return err;
    siginfo_t info = {};
    info.si_signo = SIGQUIT;
    info.si_code = SI_USER;
    info.si_uid = get_current_uid();
    intptr_t result = alpc_transport::send_to_process(pid, SIGQUIT, &info);
    return result < 0 ? -static_cast<int>(result) : 0;
  }

  default: {
    // Arbitrary signals: deliver via cross-process ALPC transport.
    siginfo_t info = {};
    info.si_signo = signum;
    info.si_code = SI_USER;
    info.si_uid = get_current_uid();
    intptr_t result = alpc_transport::send_to_process(pid, signum, &info);
    return result < 0 ? -static_cast<int>(result) : 0;
  }
  }
}

// ---------------------------------------------------------------------------
// Kill all children in a process group. Iterates the per-group chain and
// signals each member individually. Returns 0 if at least one signal was
// delivered, -ESRCH if the group is empty/nonexistent, or -errno from the
// first failed delivery (after attempting all members).
//
// Also delivers to self if the calling process is in the target group
// (POSIX: kill(0, sig) includes the caller).
// ---------------------------------------------------------------------------
static intptr_t kill_process_group(pid_t pgid, int sig) {
  // Collect PIDs under the child table lock, then signal outside the lock
  // to avoid holding it during cross-process ALPC delivery.
  constexpr int MAX_GROUP_PIDS = 256;
  pid_t pids[MAX_GROUP_PIDS];
  int count = 0;
  bool self_in_group = false;

  {
    g_pcb.child_table.lock.lock();
    count = process::collect_group_pids(pgid, pids, MAX_GROUP_PIDS);
    g_pcb.child_table.lock.unlock();
  }

  // Check if caller is in the target group.
  if (process::get_self_pgid() == pgid)
    self_in_group = true;

  // Signal 0 is an existence check — no delivery needed.
  if (sig == 0) {
    if (count > 0 || self_in_group)
      return 0;
    return -ESRCH;
  }

  if (count == 0 && !self_in_group)
    return -ESRCH;

  int first_error = 0;
  bool any_delivered = false;

  // Deliver to self first (if in group).
  if (self_in_group) {
    intptr_t self_err = deliver_process_signal(sig);
    if (self_err == 0)
      any_delivered = true;
    else if (first_error == 0)
      first_error = -static_cast<int>(self_err);
  }

  // Deliver to each child in the group.
  for (int i = 0; i < count; ++i) {
    int err = deliver_signal_to_process(static_cast<DWORD>(pids[i]), sig);
    if (err == 0)
      any_delivered = true;
    else if (first_error == 0 && err != ESRCH)
      first_error = err; // skip ESRCH — child may have exited between collect and signal
  }

  if (!any_delivered)
    return -ESRCH;
  if (first_error)
    return -first_error;
  return 0;
}

// ---------------------------------------------------------------------------
// Kernel function — implements SYS_kill semantics.
// ---------------------------------------------------------------------------
intptr_t kill(intptr_t pid_arg, int sig) {
  if (sig < 0 || sig >= NSIG)
    return -EINVAL;

  pid_t pid = static_cast<pid_t>(pid_arg);

  // Self (pid == getpid()): process-wide delivery.
  if (pid == static_cast<pid_t>(NtCurrentProcessId())) {
    if (sig == 0)
      return 0;
    return deliver_process_signal(sig);
  }

  // Process group of the caller (pid == 0): signal all children in our group.
  if (pid == 0) {
    pid_t self_pgid = process::get_self_pgid();
    return kill_process_group(self_pgid, sig);
  }

  // Negative PID: kill(-pgid, sig) → signal all children in group pgid.
  if (pid < 0) {
    pid_t target_pgid = -pid;
    return kill_process_group(target_pgid, sig);
  }

  // Specific process (pid > 0).
  int err = deliver_signal_to_process(static_cast<DWORD>(pid), sig);
  return err ? -err : 0;
}

// ---------------------------------------------------------------------------
// Kernel function — implements SYS_sigaltstack semantics.
// ---------------------------------------------------------------------------
intptr_t sigaltstack(const void *ss, void *oss) {
  return sigaltstack(static_cast<const stack_t *>(ss),
                     static_cast<stack_t *>(oss));
}

intptr_t sigaltstack(const stack_t *ss, stack_t *oss) {

  ThreadSignalState *state = get_thread_state();
  if (!state)
    return -EINVAL;

  if (oss) {
    oss->ss_sp = state->alt_stack_sp;
    oss->ss_flags = state->alt_stack_flags;
    oss->ss_size = state->alt_stack_size;
  }

  if (ss) {
    if (state->alt_stack_flags & SS_ONSTACK)
      return -EPERM;

    if (ss->ss_flags & ~(SS_DISABLE | SS_AUTODISARM))
      return -EINVAL;

    if (ss->ss_flags & SS_DISABLE) {
      state->alt_stack_sp = nullptr;
      state->alt_stack_flags = SS_DISABLE;
      state->alt_stack_size = 0;
    } else {
      if (ss->ss_size < MINSIGSTKSZ)
        return -ENOMEM;
      state->alt_stack_sp = ss->ss_sp;
      state->alt_stack_flags = ss->ss_flags & SS_AUTODISARM;
      state->alt_stack_size = ss->ss_size;
    }
  }

  return 0;
}

} // namespace signal_state

// ===========================================================================
// internal:: engine functions
// ===========================================================================

namespace internal {

// ---------------------------------------------------------------------------
// Engine function — returns -errno on failure, 0 on success.
// ---------------------------------------------------------------------------
intptr_t raise(int signum) {
  if (!signal_state::is_valid_signal(signum))
    return -EINVAL;

  signal_state::ThreadSignalState *state = signal_state::get_thread_state();
  if (!state)
    return -EAGAIN;

  // Pend the signal to the current thread and dispatch.
  if (!signal_state::pend_signal(state->pending, signum, SI_TKILL))
    return -EAGAIN;
  signal_state::signal_dispatch::trigger(state);
  signal_state::signal_dispatch::dispatch_pending(state);

  return 0;
}

// ---------------------------------------------------------------------------
// Engine function — returns -errno on failure, 0 on success.
// ---------------------------------------------------------------------------
intptr_t sigpending(sigset_t *set) {
  if (!set)
    return -EFAULT;

  signal_state::ThreadSignalState *state = signal_state::get_thread_state();

  // POSIX: report signals pending for delivery to this thread.
  uint64_t blocked = state ? signal_state::sigset_to_bits(state->blocked_signals)
                           : 0;
  uint64_t thread_pending =
      state ? (state->pending.standard.load(cpp::MemoryOrder::ACQUIRE) &
               signal_state::SIGNAL_BITS_MASK)
            : 0;
  uint64_t proc_pending =
      g_pcb.signal_dispatch.process_pending.standard.load(
          cpp::MemoryOrder::ACQUIRE);
  signal_state::bits_to_sigset(
      (thread_pending | proc_pending) & blocked, set);
  return 0;
}

// ---------------------------------------------------------------------------
// Engine function — returns -errno on failure, 0 on success.
// ---------------------------------------------------------------------------
intptr_t sigqueue(pid_t pid, int sig, const union sigval value) {
  if (sig < 0 || sig >= NSIG)
    return -EINVAL;

  // sig == 0: existence check, no signal delivered.
  if (sig == 0) {
    windows::ScopedNtHandle h;
    NTSTATUS st = ::NtOpenProcessById(h.put(), PROCESS_QUERY_LIMITED_INFORMATION,
                                      static_cast<DWORD>(pid));
    if (NT_SUCCESS(st))
      return 0;
    return (st == STATUS_ACCESS_DENIED) ? -EPERM : -ESRCH;
  }

  // SIGKILL/SIGSTOP: no payload semantics (uncatchable), behave like kill().
  if (sig == SIGKILL || sig == SIGSTOP) {
    return signal_state::kill(static_cast<intptr_t>(pid), sig);
  }

  DWORD self_pid = ::NtCurrentProcessId();

  if (static_cast<DWORD>(pid) == self_pid) {
    // Self-directed: allocate entry and pend to process-wide set.
    signal_state::SigqueueEntry *entry = signal_state::sigqueue_alloc();
    if (!entry)
      return -EAGAIN;

    entry->si_signo = sig;
    entry->si_code = SI_QUEUE;
    entry->value = value;
    entry->pid = static_cast<pid_t>(self_pid);
    entry->uid = get_current_uid();

    signal_state::signal_pending::pend_rt(
        g_pcb.signal_dispatch.process_pending, entry);
    signal_state::signal_dispatch::trigger_any_thread();
    return 0;
  }

  // Cross-process: use ALPC transport.
  siginfo_t info = {};
  info.si_signo = sig;
  info.si_code = SI_QUEUE;
  info.si_value = value;
  info.si_pid = static_cast<pid_t>(self_pid);
  info.si_uid = get_current_uid();

  intptr_t result = signal_state::alpc_transport::send_to_process(pid, sig, &info);
  return result;
}

// ---------------------------------------------------------------------------
// pthread_sigqueue(3) backend — direct thread-targeted RT signal with
// caller-supplied sigval payload. Mirrors `sigqueue` but routes by TID.
//
// Engine function — returns -errno on failure, 0 on success.
// ---------------------------------------------------------------------------
intptr_t sigqueue_thread(pid_t tid, int sig, const union sigval value) {
  if (sig < 0 || sig >= NSIG)
    return -EINVAL;
  if (tid <= 0)
    return -ESRCH;

  // sig == 0 is a presence check on Linux's rt_tgsigqueueinfo path.
  // Defer to the existing apc_transport's own sig==0 short-circuit
  // when crossing threads, or honor it inline for self.
  DWORD target_tid = static_cast<DWORD>(tid);
  bool is_self = (target_tid == ::NtCurrentThreadId());

  if (is_self) {
    if (sig == 0)
      return 0;
    signal_state::ThreadSignalState *state = signal_state::get_thread_state();
    if (!state)
      return -EAGAIN;

    // SIGKILL/SIGSTOP cannot be queued with a payload — they have no
    // catchable handler. Treat as a plain pend (matches Linux: the
    // signal is delivered, the value is dropped).
    if (sig >= SIGRTMIN && sig <= SIGRTMAX) {
      signal_state::SigqueueEntry *entry = signal_state::sigqueue_alloc();
      if (!entry)
        return -EAGAIN;
      entry->si_signo = sig;
      entry->si_code = SI_QUEUE;
      entry->value = value;
      entry->pid = static_cast<pid_t>(::NtCurrentProcessId());
      entry->uid = get_current_uid();
      signal_state::signal_pending::pend_rt(state->pending, entry);
    } else {
      // Standard signals: SI_QUEUE si_code, no payload (POSIX).
      (void)signal_state::signal_pending::pend_standard(state->pending, sig,
                                                        SI_QUEUE);
    }
    signal_state::signal_dispatch::trigger(state);
    if (!signal_state::is_signal_blocked(sig))
      signal_state::signal_dispatch::dispatch_pending(state);
    return 0;
  }

  // Cross-thread (intra-process): build a full siginfo so the APC
  // transport's pend_into can plant the RT entry with our payload.
  siginfo_t info = {};
  info.si_signo = sig;
  info.si_code = SI_QUEUE;
  info.si_value = value;
  info.si_pid = static_cast<pid_t>(::NtCurrentProcessId());
  info.si_uid = get_current_uid();

  return signal_state::apc_transport::send_to_thread(target_tid, sig, &info);
}

// ---------------------------------------------------------------------------
// Engine function — returns -errno on failure (always -EINTR on success).
// ---------------------------------------------------------------------------
intptr_t sigsuspend(const sigset_t *mask) {
  if (!mask)
    return -EFAULT;

  signal_state::ThreadSignalState *state = signal_state::get_thread_state();
  if (!state)
    return -EINTR;

  // Save current mask and install the temporary one.
  sigset_t saved_mask = state->blocked_signals;
  uint64_t tmp_mask = signal_state::sigset_to_bits(*mask) &
                      ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)) |
                        (1ULL << (__SIGRTMIN - 1)) | (1ULL << (__SIGRTMIN)));
  signal_state::bits_to_sigset(tmp_mask, &state->blocked_signals);

  state->handler_ran = false;

  // Deliver any already-pending signals that the new mask unblocks.
  uint64_t blocked = signal_state::sigset_to_bits(state->blocked_signals);
  uint64_t pending =
      (state->pending.standard.load(cpp::MemoryOrder::ACQUIRE) &
       signal_state::SIGNAL_BITS_MASK) |
      g_pcb.signal_dispatch.process_pending.standard.load(
          cpp::MemoryOrder::ACQUIRE);
  uint64_t unblocked = pending & ~blocked;
  if (unblocked) {
    signal_state::signal_dispatch::dispatch_pending(state);
    state->blocked_signals = saved_mask;
    return -EINTR;
  }

  // Register the full complement of unblocked signals as what we're waiting
  // for, so pend operations alert us via NtAlertThreadByThreadId.
  uint64_t all_unblocked = ~blocked & signal_state::SIGNAL_BITS_MASK;
  state->waiting_signals.store(all_unblocked);

  // Re-check after registering to close the pend/wait race.
  pending =
      (state->pending.standard.load(cpp::MemoryOrder::ACQUIRE) &
       signal_state::SIGNAL_BITS_MASK) |
      g_pcb.signal_dispatch.process_pending.standard.load(
          cpp::MemoryOrder::ACQUIRE);
  unblocked = pending & ~blocked;
  if (unblocked) {
    state->waiting_signals.store(0);
    signal_state::signal_dispatch::dispatch_pending(state);
    state->blocked_signals = saved_mask;
    return -EINTR;
  }

  for (;;) {
    NtWaitForAlertByThreadId(nullptr, nullptr);

    // The special user APC queued by trigger_any_thread() fires at the
    // kernel-to-user transition when NtWaitForAlertByThreadId returns.
    // That APC calls dispatch_pending(), which may have already drained
    // the pending bit and invoked the handler. Check handler_ran first
    // so we don't miss delivery that happened via APC dispatch.
    if (state->handler_ran) {
      state->waiting_signals.store(0);
      state->blocked_signals = saved_mask;
      return -EINTR;
    }

    // Cooperative stop check — test_flags is a RELAXED load (MOV),
    // zero atomics on the fast path (no active stop).
    if (state->notify_word &&
        state->notify_word->test_flags(notify::STOP)) {
      state->waiting_signals.store(0);
      signal_state::process_control::check_stop_request(state);
      state->waiting_signals.store(all_unblocked);
      continue;
    }

    blocked = signal_state::sigset_to_bits(state->blocked_signals);
    pending =
        (state->pending.standard.load() & signal_state::SIGNAL_BITS_MASK) |
        g_pcb.signal_dispatch.process_pending.standard.load();
    unblocked = pending & ~blocked;
    if (unblocked) {
      state->waiting_signals.store(0);
      signal_state::signal_dispatch::dispatch_pending(state);
      state->blocked_signals = saved_mask;
      return -EINTR;
    }
  }
}

// ---------------------------------------------------------------------------
// sigtimedwait helpers
// ---------------------------------------------------------------------------

// Try to consume one pending signal from a PendingSet that matches wait_mask.
// Uses __builtin_ctzll to jump directly to set bits instead of a linear scan.
// Returns the signal number on success, 0 if nothing consumable.
static int try_consume_from(cpp::Atomic<uint64_t> &bitmap,
                            uint64_t wait_mask, siginfo_t *info) {
  uint64_t pending =
      bitmap.load(cpp::MemoryOrder::ACQUIRE) &
      signal_state::SIGNAL_BITS_MASK;
  uint64_t ready = pending & wait_mask;

  while (ready) {
    int bit_idx = __builtin_ctzll(ready);
    uint64_t bit = 1ULL << bit_idx;
    ready &= ~bit;

    // Claim the bit atomically.
    uint64_t old = bitmap.fetch_and(~bit, cpp::MemoryOrder::ACQ_REL);
    if (!(old & bit))
      continue; // Another consumer claimed it.

    int sig = bit_idx + 1;
    if (info) {
      info->si_signo = sig;
      info->si_errno = 0;
      if (sig == SIGCHLD)
        signal_state::process_control::populate_sigchld_info(info);
      else
        info->si_code = SI_USER;
    }
    return sig;
  }
  return 0;
}

// Try to consume a pending signal that is in the wait set.
// Checks per-thread first, then process-wide.
static int try_consume_signal(signal_state::ThreadSignalState *state,
                              uint64_t wait_mask, siginfo_t *info) {
  int sig = try_consume_from(state->pending.standard, wait_mask, info);
  if (sig)
    return sig;
  return try_consume_from(g_pcb.signal_dispatch.process_pending.standard,
                          wait_mask, info);
}

// ---------------------------------------------------------------------------
// Engine function — returns signal number (>0) on success, -errno on failure.
// ---------------------------------------------------------------------------
intptr_t sigtimedwait(const sigset_t *__restrict set,
                  siginfo_t *__restrict info,
                  const struct timespec *__restrict timeout) {
  if (!set)
    return -EINVAL;

  uint64_t wait_mask = signal_state::sigset_to_bits(*set);
  if (wait_mask == 0)
    return -EINVAL;

  signal_state::ThreadSignalState *state = signal_state::get_thread_state();
  if (!state)
    return -EAGAIN;

  // Convert timeout to a QPC deadline.
  bool has_timeout = timeout != nullptr;
  LONGLONG deadline_qpc = 0;
  LONGLONG qpc_freq = 0;

  if (has_timeout) {
    LARGE_INTEGER freq, now;
    ::RtlQueryPerformanceFrequency(&freq);
    ::RtlQueryPerformanceCounter(&now);
    qpc_freq = freq.QuadPart;

    constexpr long long MAX_SEC = 9223372036LL;
    long long sec = static_cast<long long>(timeout->tv_sec);
    if (sec > MAX_SEC)
      sec = MAX_SEC;
    LONGLONG timeout_ticks =
        sec * qpc_freq +
        static_cast<long long>(timeout->tv_nsec) * qpc_freq / 1000000000LL;
    if (timeout_ticks <= 0 && (timeout->tv_sec > 0 || timeout->tv_nsec > 0))
      timeout_ticks = 1;
    deadline_qpc = now.QuadPart + timeout_ticks;
  }

  // Fast path: check if a signal is already pending.
  int sig = try_consume_signal(state, wait_mask, info);
  if (sig)
    return static_cast<intptr_t>(sig);

  // Register what we're waiting for.
  state->waiting_signals.store(wait_mask);

  // Re-check after registering to close the race.
  sig = try_consume_signal(state, wait_mask, info);
  if (sig) {
    state->waiting_signals.store(0);
    return static_cast<intptr_t>(sig);
  }

  for (;;) {
    LARGE_INTEGER nt_timeout;
    LARGE_INTEGER *timeout_ptr = nullptr;
    if (has_timeout) {
      LARGE_INTEGER now;
      ::RtlQueryPerformanceCounter(&now);
      LONGLONG remaining_ticks = deadline_qpc - now.QuadPart;
      if (remaining_ticks <= 0) {
        state->waiting_signals.store(0);
        return -EAGAIN;
      }
      LONGLONG remaining_100ns = (remaining_ticks * 10000000LL) / qpc_freq;
      if (remaining_100ns <= 0)
        remaining_100ns = 1;
      nt_timeout.QuadPart = -remaining_100ns;
      timeout_ptr = &nt_timeout;
    }

    NTSTATUS st = NtWaitForAlertByThreadId(nullptr, timeout_ptr);

    // Cooperative stop check — test_flags is a RELAXED load (MOV),
    // zero atomics on the fast path (no active stop).
    if (state->notify_word &&
        state->notify_word->test_flags(notify::STOP)) {
      state->waiting_signals.store(0);
      signal_state::process_control::check_stop_request(state);
      state->waiting_signals.store(wait_mask);
      sig = try_consume_signal(state, wait_mask, info);
      if (sig) {
        state->waiting_signals.store(0);
        return static_cast<intptr_t>(sig);
      }
      continue;
    }

    sig = try_consume_signal(state, wait_mask, info);
    if (sig) {
      state->waiting_signals.store(0);
      return static_cast<intptr_t>(sig);
    }

    if (st == STATUS_TIMEOUT) {
      state->waiting_signals.store(0);
      return -EAGAIN;
    }
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
