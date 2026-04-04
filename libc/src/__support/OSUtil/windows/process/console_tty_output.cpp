//===-- Console TTY output: OPOST, flow control, winsize -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements OPOST output translations (ONLCR/OCRNL/ONOCR/ONLRET), output
// column tracking, XON/XOFF flow control, window size queries, and the public
// console_tty::write() entry point.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/console_tty_impl.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace console_tty {
namespace {

using namespace impl;

int set_current_winsize(struct winsize const *ws) {
  if (!ws)
    return -EINVAL;

  windows::ScopedNtHandle output_handle;
  int err = open_current_console_output(output_handle);
  if (err < 0)
    return err;

  condrv::CONSOLE_SCREEN_BUFFER_INFO_EX info = {};
  NTSTATUS status =
      condrv::get_console_screen_buffer_info_ex(output_handle.get(), &info);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  const SHORT cols = static_cast<SHORT>(ws->ws_col);
  const SHORT rows = static_cast<SHORT>(ws->ws_row);
  const long right_exclusive = static_cast<long>(info.srWindow.Left) + cols;
  const long bottom_exclusive = static_cast<long>(info.srWindow.Top) + rows;
  if (right_exclusive > 0x7FFF || bottom_exclusive > 0x7FFF)
    return -EINVAL;

  // Match the host-side SetConsoleScreenBufferInfoEx contract: resize the
  // visible viewport to the requested rows/cols and ensure the backing buffer
  // is at least that large. Pixel hints in struct winsize stay advisory only.
  info.dwSize.X = cols;
  if (rows > info.dwSize.Y)
    info.dwSize.Y = rows;
  info.srWindow.Right = static_cast<SHORT>(right_exclusive);
  info.srWindow.Bottom = static_cast<SHORT>(bottom_exclusive);

  status = condrv::set_console_screen_buffer_info_ex_wrapper(output_handle.get(),
                                                             &info);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

} // anonymous namespace

// ===----------------------------------------------------------------------===
// Public API
// ===----------------------------------------------------------------------===

ssize_t write(OpenFileDescription *ofd, const void *buffer, size_t count) {
  if (!ofd || !ofd->is_console())
    return -ENOTTY;
  if (count == 0)
    return 0;

  // POSIX: write() to a terminal is a cancellation point.
  cancel_check();

  ConsoleTtyState &state = impl::tty_state();
  TerminalStateSnapshot snap;
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  impl::snapshot_terminal_state_locked(state, &snap);
  state.lock.unlock();

  int err = enforce_foreground_access(snap.has_terminal, snap.controlling_sid,
                                      snap.foreground_pgid, snap.attrs.c_lflag,
                                      TerminalAccessKind::Write);
  if (err < 0)
    return err;

  err = impl::wait_for_output_resume();
  if (err < 0)
    return err;

  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  ssize_t written = impl::write_terminal_bytes_locked(
      state, snap.attrs, ofd->handle,
      static_cast<const unsigned char *>(buffer), count);
  state.lock.unlock();
  return written;
}

int drain(int fd) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  ConsoleTtyState &state = impl::tty_state();
  err = impl::check_condrv_foreground_control(state);
  if (err < 0)
    return err;

  return impl::wait_for_output_resume();
}

int flow(int fd, int action) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  bool stop_output = false;
  switch (action) {
  case TCOOFF:
  case TCIOFF:
    stop_output = true;
    break;
  case TCOON:
  case TCION:
    stop_output = false;
    break;
  default:
    return -EINVAL;
  }

  ConsoleTtyState &state = impl::tty_state();
  err = impl::check_condrv_foreground_control(state);
  if (err < 0)
    return err;

  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  state.output_stopped = stop_output ? 1 : 0;
  impl::notify_flow_state(state);
  state.lock.unlock();
  return 0;
}

int send_break(int fd, int) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  ConsoleTtyState &state = impl::tty_state();
  err = impl::check_condrv_foreground_control(state);
  if (err < 0)
    return err;

  // ConDrv/ConPTY terminals have no physical serial line, so there is no
  // electrical break condition to transmit. However, POSIX specifies that
  // if IGNBRK is not set and BRKINT is set, a break shall generate SIGINT
  // to the foreground process group and flush both queues. Implement that
  // semantic so programs relying on tcsendbreak-as-interrupt work correctly.
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  TerminalStateSnapshot snap;
  impl::snapshot_terminal_state_locked(state, &snap);

  if (!(snap.attrs.c_iflag & IGNBRK) && (snap.attrs.c_iflag & BRKINT)) {
    impl::clear_all_buffers_locked(state);
    // Signal the foreground pgrp; fall back to process-wide if no pgrp.
    if (snap.foreground_pgid > 0) {
      (void)signal_state::kill(-static_cast<intptr_t>(snap.foreground_pgid), SIGINT);
    } else {
      signal_state::deliver_process_signal(SIGINT);
    }
  }
  // If IGNBRK is set, or neither IGNBRK nor BRKINT: successful no-op.
  state.lock.unlock();
  return 0;
}

int get_winsize(int fd, struct winsize *ws) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;
  err = impl::get_terminal_winsize(ws);
  if (err < 0)
    return err;
  observe_winsize_components(ws->ws_row, ws->ws_col, false);
  return 0;
}

int set_winsize(int fd, const struct winsize *ws) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  ConsoleTtyState &state = impl::tty_state();
  err = impl::check_condrv_foreground_control(state);
  if (err < 0)
    return err;

  err = set_current_winsize(ws);
  if (err < 0)
    return err;

  observe_winsize_components(ws->ws_row, ws->ws_col, true);
  return 0;
}

} // namespace console_tty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
