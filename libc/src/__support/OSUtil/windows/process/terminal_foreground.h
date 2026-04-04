//===-- POSIX foreground-group enforcement for terminals --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared POSIX foreground-group enforcement (SIGTTIN/SIGTTOU) used by both
// the ConDrv console backend (console_tty) and PTY backend (vt_pty) via the
// unified terminal_ops dispatch layer.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMINAL_FOREGROUND_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMINAL_FOREGROUND_H

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/pid_t.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/tcflag_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpgid.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

enum class TerminalAccessKind : uint8_t {
  Read,
  Write,
  Control,
};

LIBC_INLINE bool terminal_signal_is_ignored(int signum) {
  uint64_t bit = 1ULL << (signum - 1);
  return (g_pcb.signal_handler.ignored.load(cpp::MemoryOrder::ACQUIRE) & bit) !=
         0;
}

/// POSIX foreground-group enforcement.
///
/// Returns 0 if access is permitted, -EINTR if a job-control signal was
/// delivered, or -EIO if the signal is blocked/ignored for a read.
LIBC_INLINE int enforce_foreground_access(bool has_terminal,
                                          pid_t controlling_sid,
                                          pid_t foreground_pgid,
                                          tcflag_t local_flags,
                                          TerminalAccessKind kind) {
  if (!has_terminal)
    return 0;

  pid_t current_sid = windows_syscalls::get_session_id();
  if (current_sid != controlling_sid)
    return 0;

  pid_t current_pgid = windows_syscalls::getpgrp();
  if (foreground_pgid == 0 || current_pgid == foreground_pgid)
    return 0;

  int signum = (kind == TerminalAccessKind::Read) ? SIGTTIN : SIGTTOU;
  bool blocked = signal_state::is_signal_blocked(signum);
  bool ignored = terminal_signal_is_ignored(signum);

  if (kind == TerminalAccessKind::Read) {
    if (blocked || ignored)
      return -EIO;
    signal_state::deliver_process_signal(signum);
    return -EINTR;
  }

  if (kind == TerminalAccessKind::Write &&
      (local_flags & static_cast<tcflag_t>(TOSTOP)) == 0) {
    return 0;
  }

  if (blocked || ignored)
    return 0;

  signal_state::deliver_process_signal(signum);
  return -EINTR;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMINAL_FOREGROUND_H
