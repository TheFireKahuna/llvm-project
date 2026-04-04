//===-- Unified POSIX terminal dispatch ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single dispatch point for POSIX terminal control operations.  Routes each
// call to the appropriate backend (ConDrv console or headless-conhost PTY)
// after performing shared argument validation and foreground-group enforcement.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/terminal_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/process/pty_tree.h"
#include "src/__support/OSUtil/windows/process/terminal_foreground.h"
#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace terminal_ops {

// --- Helpers ------------------------------------------------------------ //

/// Resolve whether fd is a PTY. If so, validate and populate ofd/session.
/// Returns 0 for PTY, -ENOTTY for non-PTY (caller should try ConDrv).
static int try_resolve_pty(int fd, OpenFileDescription **ofd_out,
                           vt_pty::Session **session_out) {
  if (vt_pty::is_terminal_fd(fd) != 0)
    return -ENOTTY;
  return vt_pty::validate_pty_fd(fd, ofd_out, session_out);
}

/// Returns true when the OFD represents a PTY slave (not master).
static bool is_slave_ofd(OpenFileDescription *ofd) {
  return ofd && ofd->is_pty_slave();
}

static int validate_winsize_request(const struct winsize *ws) {
  if (!ws)
    return -EINVAL;

  // POSIX allows 0-dimension windows — programs use {0,0} to signal "unknown
  // terminal size". Linux accepts this, so we must too. The only hard ceiling
  // is ConDrv's signed 16-bit coordinate space.
  constexpr unsigned MAX_TERMINAL_DIMENSION = 0x7FFEu;
  if (ws->ws_row > MAX_TERMINAL_DIMENSION || ws->ws_col > MAX_TERMINAL_DIMENSION)
    return -EINVAL;

  return 0;
}

// --- Dispatch ----------------------------------------------------------- //

int is_terminal_fd(int fd) {
  if (vt_pty::is_terminal_fd(fd) == 0)
    return 0;
  return console_tty::is_terminal_fd(fd);
}

int get_attr(int fd, struct termios *t) {
  if (!t)
    return -EINVAL;

  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0)
    return vt_pty::pty_get_attr(session, t);

  return console_tty::get_attr(fd, t);
}

int set_attr(int fd, int actions, const struct termios *t) {
  if (!t)
    return -EINVAL;
  switch (actions) {
  case TCSANOW:
  case TCSADRAIN:
  case TCSAFLUSH:
    break;
  default:
    return -EINVAL;
  }

  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      int err =
          vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_set_attr(session, actions, t);
  }

  return console_tty::set_attr(fd, actions, t);
}

int flush(int fd, int queue_selector) {
  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      int err =
          vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_flush(session, queue_selector);
  }

  return console_tty::flush(fd, queue_selector);
}

int drain(int fd) {
  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      int err =
          vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_drain(session);
  }

  return console_tty::drain(fd);
}

ErrorOr<pid_t> get_sid(int fd) {
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, nullptr, &session) == 0)
    return vt_pty::pty_get_sid(session);

  return console_tty::get_sid(fd);
}

ErrorOr<pid_t> get_foreground_pgrp(int fd) {
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, nullptr, &session) == 0)
    return vt_pty::pty_get_foreground_pgrp(session);

  return console_tty::get_foreground_pgrp(fd);
}

int set_foreground_pgrp(int fd, pid_t pgid) {
  if (pgid <= 0)
    return -EINVAL;

  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      int err =
          vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_set_foreground_pgrp(session, pgid);
  }

  return console_tty::set_foreground_pgrp(fd, pgid);
}

int flow(int fd, int action) {
  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      int err =
          vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_flow(session, action);
  }

  return console_tty::flow(fd, action);
}

int send_break(int fd, int duration) {
  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      int err =
          vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_send_break(session);
  }

  return console_tty::send_break(fd, duration);
}

int get_pending_input_bytes(int fd, int *count) {
  if (!count)
    return -EINVAL;

  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0)
    return vt_pty::pty_get_pending_input_bytes(session, count);

  return console_tty::get_pending_input_bytes(fd, count);
}

int get_winsize(int fd, struct winsize *ws) {
  if (!ws)
    return -EINVAL;

  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, nullptr, &session) == 0)
    return vt_pty::pty_get_winsize(session, ws);

  return console_tty::get_winsize(fd, ws);
}

int set_winsize(int fd, const struct winsize *ws) {
  int err = validate_winsize_request(ws);
  if (err < 0)
    return err;

  OpenFileDescription *ofd = nullptr;
  vt_pty::Session *session = nullptr;
  if (try_resolve_pty(fd, &ofd, &session) == 0) {
    if (is_slave_ofd(ofd)) {
      err = vt_pty::check_foreground_access(session, TerminalAccessKind::Control);
      if (err < 0)
        return err;
    }
    return vt_pty::pty_set_winsize(session, ws);
  }

  return console_tty::set_winsize(fd, ws);
}

void detach_controlling_terminal() {
  console_tty::detach_controlling_terminal();
  vt_pty::clear_current_attachment_keepalive();
  pty_tree::clear_current_attached_pty();
}

} // namespace terminal_ops
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
