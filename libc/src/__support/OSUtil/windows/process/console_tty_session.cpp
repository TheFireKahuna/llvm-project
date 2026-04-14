//===-- Console TTY session: ownership, foreground, termios control -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements terminal ownership (session/pgrp), foreground process group
// enforcement, adopt/detach, fork reinit, and the public termios
// get/set/flush control plane. All shared-state access uses the dispatch
// helpers (snapshot_terminal_state_locked, set_terminal_attrs, etc.) so the
// PTY-attached vs. standalone branching is centralized in console_tty_impl.h.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/console_tty_impl.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace console_tty {

// ===----------------------------------------------------------------------===
// Public API: validation
// ===----------------------------------------------------------------------===

int validate_console_fd(int fd, OpenFileDescription **ofd_out) {
  return impl::validate_console_handle_fd(fd, ofd_out);
}

int is_terminal_fd(int fd) {
  return impl::validate_console_handle_fd(fd, nullptr);
}

// ===----------------------------------------------------------------------===
// Public API: termios get/set
// ===----------------------------------------------------------------------===

int get_attr(int fd, struct termios *t) {
  if (!t)
    return -EINVAL;

  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  impl::snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();

  *t = snap.attrs;
  return 0;
}

int set_attr(int fd, int actions, const struct termios *t) {
  if (!t)
    return -EINVAL;

  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  switch (actions) {
  case TCSANOW:
  case TCSADRAIN:
  case TCSAFLUSH:
    break;
  default:
    return -EINVAL;
  }

  // Foreground access check via snapshot.
  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  impl::snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();

  err = enforce_foreground_access(snap.has_terminal, snap.controlling_sid,
                                  snap.foreground_pgid, 0,
                                  TerminalAccessKind::Control);
  if (err < 0)
    return err;

  // Authority-first ordering: write the source of truth before pushing
  // ConDrv modes. For PTY: PtySharedState under kernel mutant. For
  // standalone: ConsoleTtyState.attrs under lock. A concurrent reader
  // always sees the latest attrs from the authority.
  err = impl::set_terminal_attrs(t);
  if (err < 0)
    return err;

  err = impl::apply_console_modes(*t);
  if (err < 0)
    return err;

  if (actions == TCSAFLUSH) {
    state.lock.lock();
    impl::clear_all_buffers_locked(state);
    state.lock.unlock();

    windows::ScopedNtHandle input_handle;
    err = impl::open_current_console_input(input_handle);
    if (err < 0)
      return err;
    NTSTATUS status = condrv::flush_console_input(input_handle.get());
    if (!NT_SUCCESS(status))
      return -windows_util::ntstatus_to_errno(status);
  }

  return 0;
}

// ===----------------------------------------------------------------------===
// Public API: tcflush
// ===----------------------------------------------------------------------===

int flush(int fd, int queue_selector) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  ConsoleTtyState &state = impl::tty_state();
  err = impl::check_condrv_foreground_control(state);
  if (err < 0)
    return err;

  switch (queue_selector) {
  case TCIFLUSH:
  case TCIOFLUSH: {
    state.lock.lock();
    impl::clear_all_buffers_locked(state);
    state.lock.unlock();

    windows::ScopedNtHandle input_handle;
    err = impl::open_current_console_input(input_handle);
    if (err < 0)
      return err;
    NTSTATUS status = condrv::flush_console_input(input_handle.get());
    if (!NT_SUCCESS(status))
      return -windows_util::ntstatus_to_errno(status);
    return 0;
  }
  case TCOFLUSH:
    return 0;
  default:
    return -EINVAL;
  }
}

// ===----------------------------------------------------------------------===
// Public API: session ID and foreground process group
// ===----------------------------------------------------------------------===

ErrorOr<pid_t> get_sid(int fd) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return Error(-err);

  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  impl::snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();

  if (!snap.has_terminal || snap.controlling_sid == 0)
    return Error(ENOTTY);

  pid_t current_sid = windows_syscalls::get_session_id();
  if (current_sid != snap.controlling_sid)
    return Error(ENOTTY);
  return snap.controlling_sid;
}

ErrorOr<pid_t> get_foreground_pgrp(int fd) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return Error(-err);

  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  impl::snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();

  if (!snap.has_terminal || snap.controlling_sid == 0)
    return Error(ENOTTY);

  pid_t current_sid = windows_syscalls::get_session_id();
  if (current_sid != snap.controlling_sid)
    return Error(ENOTTY);
  return snap.foreground_pgid;
}

// POSIX-inherent TOCTOU: we unlock state, perform external validation
// (foreground access check, process group existence), then re-snapshot and
// re-validate before committing. This mirrors the Linux kernel's
// tty_check_change() + tiocspgrp() pattern — the gap between validation
// and commit is intrinsic to the POSIX API contract. The re-validation
// on the second snapshot catches races where the terminal was detached or
// the session changed during the unlocked window.
int set_foreground_pgrp(int fd, pid_t pgid) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;
  if (pgid <= 0)
    return -EINVAL;

  // First snapshot: read current state for validation.
  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  impl::snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();

  pid_t current_sid = windows_syscalls::get_session_id();
  if (!snap.has_terminal || snap.controlling_sid == 0 ||
      current_sid != snap.controlling_sid)
    return -ENOTTY;

  err = enforce_foreground_access(snap.has_terminal, snap.controlling_sid,
                                  snap.foreground_pgid, 0,
                                  TerminalAccessKind::Control);
  if (err < 0)
    return err;

  pid_t current_pgid = windows_syscalls::getpgrp();
  if (pgid != current_pgid) {
    g_pcb.child_table.lock.lock();
    bool group_exists = process::has_process_group(pgid);
    g_pcb.child_table.lock.unlock();
    if (!group_exists)
      return -EPERM;
  }

  // Re-snapshot under lock to catch races (terminal detached, session changed).
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  impl::snapshot_terminal_state_locked(state, &snap);
  if (snap.controlling_sid != current_sid || !snap.has_terminal) {
    state.lock.unlock();
    return -ENOTTY;
  }
  state.lock.unlock();

  // Write to authority AFTER all validation — fixes the previous bug where
  // PtySharedState was written before validation, allowing invalid pgid
  // values to reach the shared state on validation failure.
  return impl::set_terminal_foreground_pgrp(pgid);
}

// ===----------------------------------------------------------------------===
// Public API: terminal adopt/detach and fork reinit
// ===----------------------------------------------------------------------===

void adopt_controlling_terminal() {
  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  impl::adopt_current_terminal_locked(state);
  state.lock.unlock();
}

void detach_controlling_terminal() {
  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  impl::detach_terminal_locked(state);
  state.lock.unlock();
}

void console_tty_fork_reinit() {
  auto &state = impl::tty_state();
  state.lock.reset_for_fork();
  state.initialized.store(0, cpp::MemoryOrder::RELAXED);
  impl::clear_all_buffers_locked(state);
  state.has_terminal = 0;
  state.controlling_sid = 0;
  state.foreground_pgid = 0;
  state.output_stopped = 0;
  // Post-fork single-threaded: reset both futex value and stale Treiber
  // wait stack (parent waiters don't exist in the child).
  state.flow_wait.reset_for_fork(0);
}

} // namespace console_tty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
void LIBC_NAMESPACE::internal::console_tty_fork_reinit() {
  LIBC_NAMESPACE::internal::console_tty::console_tty_fork_reinit();
}
