//===-- Windows implementation of ioctl ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/ioctl/ioctl.h"

#include "hdr/types/pid_t.h"
#include "hdr/types/struct_winsize.h"
#include "src/__support/OSUtil/windows/process/terminal_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"

#include <stdarg.h>
#include <sys/ioctl.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, ioctl, (int fd, unsigned long request, ...)) {
  va_list vargs;
  va_start(vargs, request);

  int ret;
  switch (request) {
  case FIONREAD: {
    auto *count = va_arg(vargs, int *);
    va_end(vargs);
    ret = internal::terminal_ops::get_pending_input_bytes(fd, count);
    if (ret < 0) {
      libc_errno = -ret;
      return -1;
    }
    return 0;
  }
  case TIOCGPGRP: {
    auto *pgid = va_arg(vargs, pid_t *);
    va_end(vargs);
    if (!pgid) {
      libc_errno = EINVAL;
      return -1;
    }
    auto result = internal::terminal_ops::get_foreground_pgrp(fd);
    if (!result.has_value()) {
      libc_errno = result.error();
      return -1;
    }
    *pgid = result.value();
    return 0;
  }
  case TIOCSPGRP: {
    auto *pgid = va_arg(vargs, const pid_t *);
    va_end(vargs);
    if (!pgid) {
      libc_errno = EINVAL;
      return -1;
    }
    ret = internal::terminal_ops::set_foreground_pgrp(fd, *pgid);
    if (ret < 0) {
      libc_errno = -ret;
      return -1;
    }
    return 0;
  }
  case TIOCSCTTY: {
    // arg is an integer flag (non-zero = steal from other session).
    // We don't support stealing — always acquire if no controller exists.
    (void)va_arg(vargs, int);
    va_end(vargs);
    // Verify fd is a terminal before attempting acquisition.
    ret = internal::terminal_ops::is_terminal_fd(fd);
    if (ret < 0) {
      libc_errno = -ret;
      return -1;
    }
    // Actual session attachment is handled by setsid() + first open;
    // TIOCSCTTY as a standalone ioctl succeeds if the fd is already our
    // controlling terminal, or if we have no controlling terminal yet.
    // Full POSIX semantics: caller must be session leader with no ctty.
    // For now, succeed unconditionally for terminal fds — the session
    // model enforces the real constraints at open/setsid time.
    return 0;
  }
  case TIOCNOTTY: {
    va_end(vargs);
    internal::terminal_ops::detach_controlling_terminal();
    return 0;
  }
  case TIOCGETD: {
    auto *ldisc = va_arg(vargs, int *);
    va_end(vargs);
    if (!ldisc) {
      libc_errno = EINVAL;
      return -1;
    }
    ret = internal::terminal_ops::is_terminal_fd(fd);
    if (ret < 0) {
      libc_errno = -ret;
      return -1;
    }
    // Always N_TTY (0) — the only line discipline we implement.
    *ldisc = 0;
    return 0;
  }
  case TIOCGWINSZ: {
    auto *ws = va_arg(vargs, struct winsize *);
    va_end(vargs);
    ret = internal::terminal_ops::get_winsize(fd, ws);
    if (ret < 0) {
      libc_errno = -ret;
      return -1;
    }
    return 0;
  }
  case TIOCSWINSZ: {
    auto *ws = va_arg(vargs, struct winsize *);
    va_end(vargs);
    ret = internal::terminal_ops::set_winsize(fd, ws);
    if (ret < 0) {
      libc_errno = -ret;
      return -1;
    }
    return 0;
  }
  default:
    va_end(vargs);
    libc_errno = ENOTTY;
    return -1;
  }
}

} // namespace LIBC_NAMESPACE_DECL
