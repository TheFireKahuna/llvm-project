//===-- Shared internal helpers for console_tty modules ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal implementation header included by console_tty_input.cpp,
// console_tty_output.cpp, and console_tty_session.cpp. Contains all shared
// LIBC_INLINE helpers that multiple modules depend on.
//
// NOT a public header — do not include from outside this module.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_IMPL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_IMPL_H

#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/process/termios_defaults.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/unistd_macros.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/console_file_io.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/process/pty_tree.h"
#include "src/__support/OSUtil/windows/process/terminal_foreground.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/handler_table.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpgid.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/process/windows/child_table.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace console_tty {
namespace impl {

// ===----------------------------------------------------------------------===
// Constants
// ===----------------------------------------------------------------------===

inline constexpr DWORD MANAGED_INPUT_MODE_BITS =
    condrv::ENABLE_PROCESSED_INPUT | condrv::ENABLE_LINE_INPUT |
    condrv::ENABLE_ECHO_INPUT;
inline constexpr DWORD MANAGED_OUTPUT_MODE_BITS =
    condrv::ENABLE_PROCESSED_OUTPUT |
    condrv::ENABLE_VIRTUAL_TERMINAL_PROCESSING;
inline constexpr LONGLONG DECISECOND_IN_100NS = 1000000LL;
inline constexpr unsigned CONSOLE_TAB_WIDTH = 8u;
inline constexpr unsigned CONSOLE_TAB_MASK = CONSOLE_TAB_WIDTH - 1u;

using termios_defaults::desired_input_mode;
using termios_defaults::desired_output_mode;

// Forward declaration — defined later in this header (flow control section).
LIBC_INLINE void notify_flow_state(ConsoleTtyState &state);

// ===----------------------------------------------------------------------===
// Forward declarations (mutual dependencies between helpers)
// ===----------------------------------------------------------------------===

LIBC_INLINE void ensure_state_initialized_locked(ConsoleTtyState &state);

// ===----------------------------------------------------------------------===
// State accessor
// ===----------------------------------------------------------------------===

LIBC_INLINE ConsoleTtyState &tty_state() { return g_pcb.console_tty; }

// ===----------------------------------------------------------------------===
// Fd validation
// ===----------------------------------------------------------------------===

LIBC_INLINE int validate_console_handle_fd(int fd,
                                           OpenFileDescription **ofd_out) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;
  if (!ofd->is_console())
    return -ENOTTY;
  if (ofd_out)
    *ofd_out = ofd;
  return 0;
}

// ===----------------------------------------------------------------------===
// Console handle open helpers
// ===----------------------------------------------------------------------===

LIBC_INLINE int open_current_console_input(windows::ScopedNtHandle &handle) {
  HANDLE raw = nullptr;
  NTSTATUS status = condrv::open_condrv_absolute(
      &raw, condrv::CONDRV_CURRENT_INPUT_PATH,
      FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  handle.reset(raw);
  return 0;
}

LIBC_INLINE int open_current_console_output(windows::ScopedNtHandle &handle) {
  HANDLE raw = nullptr;
  NTSTATUS status = condrv::open_condrv_absolute(
      &raw, condrv::CONDRV_CURRENT_OUTPUT_PATH,
      FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  handle.reset(raw);
  return 0;
}

// ===----------------------------------------------------------------------===
// Termios / mode conversion
// ===----------------------------------------------------------------------===

LIBC_INLINE void fill_termios_from_modes(DWORD input_mode, DWORD output_mode,
                                         struct termios *t) {
  *t = {};
  t->c_cflag = static_cast<tcflag_t>(
      CREAD | CS8 | CLOCAL | termios_defaults::DEFAULT_TERMINAL_SPEED);

  if (input_mode & condrv::ENABLE_PROCESSED_INPUT) {
    t->c_iflag |= static_cast<tcflag_t>(BRKINT | ICRNL);
    t->c_lflag |= static_cast<tcflag_t>(ISIG);
  }
  if (input_mode & condrv::ENABLE_LINE_INPUT) {
    t->c_iflag |= static_cast<tcflag_t>(ICRNL);
    t->c_lflag |= static_cast<tcflag_t>(ICANON);
  }
  if (input_mode & condrv::ENABLE_ECHO_INPUT)
    t->c_lflag |= static_cast<tcflag_t>(ECHO | ECHOE | ECHOK);
  if (output_mode & condrv::ENABLE_PROCESSED_OUTPUT)
    t->c_oflag |= static_cast<tcflag_t>(OPOST | ONLCR);

  termios_defaults::set_default_control_chars(t);
}

LIBC_INLINE int query_current_modes(DWORD *input_mode, DWORD *output_mode) {
  windows::ScopedNtHandle input_handle;
  windows::ScopedNtHandle output_handle;

  int err = open_current_console_input(input_handle);
  if (err < 0)
    return err;
  err = open_current_console_output(output_handle);
  if (err < 0)
    return err;

  NTSTATUS status = condrv::get_console_mode(input_handle.get(), input_mode);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  status = condrv::get_console_mode(output_handle.get(), output_mode);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

// ===----------------------------------------------------------------------===
// Buffer management
// ===----------------------------------------------------------------------===

LIBC_INLINE void clear_ready_locked(ConsoleTtyState &state) {
  state.ready_offset = 0;
  state.ready_size = 0;
}

LIBC_INLINE void clear_canonical_locked(ConsoleTtyState &state) {
  state.canonical_size = 0;
}

LIBC_INLINE void clear_all_buffers_locked(ConsoleTtyState &state) {
  clear_ready_locked(state);
  clear_canonical_locked(state);
}

LIBC_INLINE size_t ready_available_locked(const ConsoleTtyState &state) {
  return static_cast<size_t>(state.ready_size - state.ready_offset);
}

// ===----------------------------------------------------------------------===
// Terminal state dispatch helpers
// ===----------------------------------------------------------------------===
//
// These helpers abstract the PTY-attached vs. standalone-console state access
// pattern. Instead of scattering `has_current_attached_pty()` checks across
// every public function, the check is centralized here:
//
//   snapshot_terminal_state_locked — reads attrs/sid/pgid from the authority
//   set_terminal_attrs             — writes attrs to the authority
//   set_terminal_foreground_pgrp   — writes foreground pgid to the authority
//   get_terminal_winsize           — reads winsize from the authority
//
// For standalone console: ConsoleTtyState fields are the authority.
// For PTY-attached: PtySharedState (seqlock) is the authority.
// ===----------------------------------------------------------------------===

/// Read a consistent snapshot of shared terminal state.
/// Called UNDER state.lock. For standalone mode, copies directly from
/// ConsoleTtyState fields. For PTY-attached mode, performs a zero-syscall
/// seqlock read from PtySharedState (PAGE_READONLY mapped view) with
/// fallback to local state on seqlock failure.
LIBC_INLINE void
snapshot_terminal_state_locked(ConsoleTtyState &state,
                               TerminalStateSnapshot *out) {
  if (pty_tree::has_current_attached_pty()) {
    pty_tree::SyncSnapshot sync = {};
    if (pty_tree::current_get_sync_snapshot(&sync) == 0) {
      out->attrs = sync.attrs;
      out->controlling_sid = sync.controlling_sid;
      out->foreground_pgid = sync.foreground_pgrp;
      out->has_terminal = state.has_terminal != 0;
      return;
    }
    // Seqlock read failed (torn read or unmapped) — fall through to local.
  }
  out->attrs = state.attrs;
  out->controlling_sid = state.controlling_sid;
  out->foreground_pgid = state.foreground_pgid;
  out->has_terminal = state.has_terminal != 0;
}

/// Write termios attrs to the authority. Called WITHOUT state.lock held.
/// Authority-first ordering: the source of truth is updated before ConDrv
/// mode pushing, so a concurrent reader always sees the latest attrs.
LIBC_INLINE int set_terminal_attrs(const struct termios *attrs) {
  if (pty_tree::has_current_attached_pty())
    return pty_tree::current_set_attr(attrs);
  // Standalone: ConsoleTtyState.attrs is the authority.
  ConsoleTtyState &state = tty_state();
  state.lock.lock();
  state.attrs = *attrs;
  state.lock.unlock();
  return 0;
}

/// Write foreground process group to the authority.
/// Called WITHOUT state.lock held, after all POSIX validation.
LIBC_INLINE int set_terminal_foreground_pgrp(pid_t pgid) {
  if (pty_tree::has_current_attached_pty())
    return pty_tree::current_set_foreground_pgrp(pgid);
  ConsoleTtyState &state = tty_state();
  state.lock.lock();
  state.foreground_pgid = pgid;
  state.lock.unlock();
  return 0;
}

/// Query window size from the authority.
LIBC_INLINE int get_terminal_winsize(struct winsize *ws) {
  if (!ws)
    return -EINVAL;
  if (pty_tree::has_current_attached_pty()) {
    int err = pty_tree::current_get_winsize(ws);
    if (err == 0)
      return 0;
    // Fall through to ConDrv query on failure.
  }
  windows::ScopedNtHandle output_handle;
  int err = open_current_console_output(output_handle);
  if (err < 0)
    return err;
  condrv::CONSOLE_SCREEN_BUFFER_INFO_EX info = {};
  NTSTATUS status =
      condrv::get_console_screen_buffer_info_ex(output_handle.get(), &info);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  console_util::srwindow_to_rowcol(info.srWindow, &ws->ws_row, &ws->ws_col);
  ws->ws_xpixel = 0;
  ws->ws_ypixel = 0;
  return 0;
}

// ===----------------------------------------------------------------------===
// Session / terminal ownership
// ===----------------------------------------------------------------------===

LIBC_INLINE void adopt_current_terminal_locked(ConsoleTtyState &state) {
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (!params || !params->ConsoleHandle) {
    state.has_terminal = 0;
    state.controlling_sid = 0;
    state.foreground_pgid = 0;
    state.output_stopped = 0;
    notify_flow_state(state);
    state.output_column = 0;
    return;
  }

  state.has_terminal = 1;
  // Use the dispatch helper to read sid/pgid from the authority (PTY shared
  // state or local). For standalone mode the snapshot returns the local
  // fields (which are zero at init time), so we fall through to process
  // identity. For PTY mode the snapshot returns PtySharedState values.
  TerminalStateSnapshot snap = {};
  snapshot_terminal_state_locked(state, &snap);
  state.controlling_sid = snap.controlling_sid > 0
                              ? snap.controlling_sid
                              : windows_syscalls::get_session_id();
  state.foreground_pgid = snap.foreground_pgid > 0
                              ? snap.foreground_pgid
                              : windows_syscalls::getpgrp();
  state.output_stopped = 0;
  notify_flow_state(state);
  state.output_column = 0;
}

LIBC_INLINE void detach_terminal_locked(ConsoleTtyState &state) {
  clear_all_buffers_locked(state);
  state.has_terminal = 0;
  state.controlling_sid = 0;
  state.foreground_pgid = 0;
  state.output_stopped = 0;
  notify_flow_state(state);
  state.output_column = 0;
}

// ===----------------------------------------------------------------------===
// Foreground access enforcement
// ===----------------------------------------------------------------------===

/// Foreground-access check for ConDrv control operations (set_attr, flush,
/// drain, flow, send_break). Snapshots shared state under lock and delegates
/// to the shared enforce_foreground_access().
LIBC_INLINE int check_condrv_foreground_control(ConsoleTtyState &state) {
  state.lock.lock();
  ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();
  return enforce_foreground_access(snap.has_terminal, snap.controlling_sid,
                                   snap.foreground_pgid, 0,
                                   TerminalAccessKind::Control);
}

// ===----------------------------------------------------------------------===
// Lazy initialization
// ===----------------------------------------------------------------------===

LIBC_INLINE void ensure_state_initialized_locked(ConsoleTtyState &state) {
  if (state.initialized.load(cpp::MemoryOrder::ACQUIRE) != 0)
    return;

  DWORD input_mode = 0;
  DWORD output_mode = 0;
  if (query_current_modes(&input_mode, &output_mode) == 0)
    fill_termios_from_modes(input_mode, output_mode, &state.attrs);
  else {
    state.attrs = {};
    state.attrs.c_cflag =
        static_cast<tcflag_t>(CREAD | CS8 | CLOCAL |
                              termios_defaults::DEFAULT_TERMINAL_SPEED);
    state.attrs.c_iflag = static_cast<tcflag_t>(ICRNL | BRKINT | IXON);
    state.attrs.c_oflag = static_cast<tcflag_t>(OPOST | ONLCR);
    state.attrs.c_lflag =
        static_cast<tcflag_t>(ISIG | ICANON | ECHO | ECHOE | ECHOK);
    termios_defaults::set_default_control_chars(&state.attrs);
  }

  // If PTY-attached, override attrs from the shared authority. The
  // snapshot reads PtySharedState via the seqlock; for standalone mode
  // it reads back the attrs we just set above — a harmless identity copy.
  TerminalStateSnapshot snap = {};
  snapshot_terminal_state_locked(state, &snap);
  state.attrs = snap.attrs;

  clear_all_buffers_locked(state);
  adopt_current_terminal_locked(state);
  state.initialized.store(1, cpp::MemoryOrder::RELEASE);
}

// ===----------------------------------------------------------------------===
// Console mode application
// ===----------------------------------------------------------------------===

LIBC_INLINE int apply_console_modes(const struct termios &attrs) {
  windows::ScopedNtHandle input_handle;
  windows::ScopedNtHandle output_handle;

  int err = open_current_console_input(input_handle);
  if (err < 0)
    return err;
  err = open_current_console_output(output_handle);
  if (err < 0)
    return err;

  DWORD input_mode = 0;
  DWORD output_mode = 0;
  NTSTATUS status = condrv::get_console_mode(input_handle.get(), &input_mode);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  status = condrv::get_console_mode(output_handle.get(), &output_mode);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  input_mode &= ~MANAGED_INPUT_MODE_BITS;
  input_mode |= desired_input_mode(attrs);

  output_mode &= ~MANAGED_OUTPUT_MODE_BITS;
  output_mode |= desired_output_mode(attrs);

  status = condrv::set_console_mode(input_handle.get(), input_mode);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // NOTE: If this fails, input mode has already been applied — the console is
  // left with new input modes but old output modes. No rollback is attempted;
  // real terminals behave similarly (partial mode application is observable).
  status = condrv::set_console_mode(output_handle.get(), output_mode);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

// ===----------------------------------------------------------------------===
// Output column tracking and OPOST write path
// ===----------------------------------------------------------------------===

LIBC_INLINE void note_output_byte_locked(ConsoleTtyState &state,
                                         const struct termios &attrs,
                                         unsigned char byte) {
  switch (byte) {
  case '\r':
    state.output_column = 0;
    return;
  case '\n':
    if ((attrs.c_oflag & ONLRET) ||
        ((attrs.c_oflag & OPOST) && (attrs.c_oflag & ONLCR)))
      state.output_column = 0;
    return;
  case '\b':
    if (state.output_column != 0)
      --state.output_column;
    return;
  case '\t': {
    unsigned col = state.output_column;
    unsigned next_tab = (col + CONSOLE_TAB_WIDTH) & ~CONSOLE_TAB_MASK;
    state.output_column =
        next_tab > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(next_tab);
    return;
  }
  default:
    break;
  }

  // TODO(UTF-8): proper column tracking requires decoding multi-byte sequences
  // to codepoints, then consulting East Asian Width (UAX #11) for wcwidth().
  // Currently advances by one column per byte >= 0x20, which over-counts for
  // multi-byte codepoints and under-counts for wide/fullwidth characters.
  if (byte >= 0x20 && byte != 0x7F && state.output_column != 0xFFFFu)
    ++state.output_column;
}

LIBC_INLINE ssize_t write_terminal_bytes_locked(ConsoleTtyState &state,
                                                const struct termios &attrs,
                                                HANDLE handle,
                                                const unsigned char *buffer,
                                                size_t count) {
  for (size_t i = 0; i < count; ++i) {
    unsigned char byte = buffer[i];
    unsigned char translated[2] = {};
    const unsigned char *to_write = &byte;
    size_t write_count = 1;

    if (attrs.c_oflag & OPOST) {
      if ((attrs.c_oflag & ONOCR) && byte == '\r' && state.output_column == 0)
        continue;

      if ((attrs.c_oflag & OCRNL) && byte == '\r') {
        translated[0] = '\n';
        to_write = translated;
      } else if ((attrs.c_oflag & ONLCR) && byte == '\n') {
        translated[0] = '\r';
        translated[1] = '\n';
        to_write = translated;
        write_count = 2;
      }
    }

    ssize_t written = console_file_io::write(handle, to_write, write_count);
    if (written < 0)
      return (i != 0) ? static_cast<ssize_t>(i) : written;
    // NOTE: If ONLCR expands \n to \r\n and only \r is written (partial),
    // we report i bytes consumed. On retry the caller resends \n which
    // re-expands to \r\n, producing a spurious \r. This is a known
    // limitation shared with Linux terminal drivers and is not easily
    // fixable without tracking inter-call output state.
    if (static_cast<size_t>(written) != write_count)
      return static_cast<ssize_t>(i);

    for (size_t emitted = 0; emitted < write_count; ++emitted)
      note_output_byte_locked(state, attrs, to_write[emitted]);
  }
  return static_cast<ssize_t>(count);
}

// ===----------------------------------------------------------------------===
// XON/XOFF flow control wait
// ===----------------------------------------------------------------------===

// Update flow_wait futex to mirror output_stopped and wake any blocked
// output threads. Must be called with state.lock held after writing
// output_stopped.
LIBC_INLINE void notify_flow_state(ConsoleTtyState &state) {
  if (state.output_stopped)
    state.flow_wait.store(1, cpp::MemoryOrder::RELEASE);
  else
    state.flow_wait.store_and_notify_all(0);
}

LIBC_INLINE int wait_for_output_resume_locked(ConsoleTtyState &state) {
  for (;;) {
    if (state.output_stopped == 0)
      return 0;

    state.lock.unlock();
    // Block on the futex until output_stopped is cleared (XON received).
    // The wait is alertable — signal delivery wakes us via
    // NtAlertThreadByThreadId through the futex wait_slot infrastructure.
    long wait_err = state.flow_wait.wait(1, cpp::nullopt);
    state.lock.lock();

    if (wait_err == -EINTR || wait_err == -EAGAIN) {
      // Woken by signal delivery or spurious wake — check for cancellation
      // and whether the interrupted syscall should restart.
      cancel_check();
      auto *tss = signal_state::get_thread_state_noinit();
      if (tss)
        tss->handler_ran = false;
      if (signal_state::should_restart_syscall())
        continue;
      if (tss && !tss->handler_ran)
        continue;
      return -EINTR;
    }
    // Any other negative return (e.g. -ENOMEM from wait-slot pool
    // exhaustion) is unrecoverable here — surface to the caller as
    // errno. Looping would re-fail identically.
    if (wait_err < 0)
      return static_cast<int>(wait_err);
    // Spurious wakes (wait returns 0 but output_stopped still set) are
    // handled by the loop re-checking the condition.
  }
}

LIBC_INLINE int wait_for_output_resume() {
  ConsoleTtyState &state = tty_state();
  state.lock.lock();
  int result = wait_for_output_resume_locked(state);
  state.lock.unlock();
  return result;
}

} // namespace impl
} // namespace console_tty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_IMPL_H
