//===-- Unified POSIX terminal dispatch layer -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single dispatch point for all POSIX terminal control operations.  Owns
// argument validation and foreground-group enforcement, then delegates to the
// appropriate backend:
//
//   - console_tty  (ConDrv-backed real console)
//   - vt_pty       (headless conhost PTY)
//
// read/write are NOT dispatched here — they go through io/read_write.cpp
// based on OpenFileDescription type flags (console_tty::read/write handle
// line discipline for both ConDrv and PTY-slave fds).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMINAL_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMINAL_OPS_H

#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/struct_winsize.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

struct termios;

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace terminal_ops {

/// Returns 0 if fd refers to any terminal (console or PTY), -ENOTTY otherwise.
int is_terminal_fd(int fd);

int get_attr(int fd, struct termios *t);
int set_attr(int fd, int actions, const struct termios *t);
int flush(int fd, int queue_selector);
int drain(int fd);
ErrorOr<pid_t> get_sid(int fd);
ErrorOr<pid_t> get_foreground_pgrp(int fd);
int set_foreground_pgrp(int fd, pid_t pgid);
int flow(int fd, int action);
int send_break(int fd, int duration);
int get_pending_input_bytes(int fd, int *count);
int get_winsize(int fd, struct winsize *ws);
int set_winsize(int fd, const struct winsize *ws);

/// Detach the calling process from its controlling terminal.  Orchestrates
/// cleanup across all terminal backends (ConDrv state, PTY keepalive,
/// pty_tree attachment).
void detach_controlling_terminal();

} // namespace terminal_ops
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMINAL_OPS_H
