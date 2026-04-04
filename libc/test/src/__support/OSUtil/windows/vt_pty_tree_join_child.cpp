//===-- Tree-scoped PTY join: child helper --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Standalone child process for vt_pty_tree_join_test. Reads a PTY slave path
// from fd 3, opens it, and validates terminal properties (isatty, tcgetattr,
// TIOCGWINSZ). Exit code 0 = success, non-zero = failure stage.
//
// All libc entry points are invoked via LIBC_NAMESPACE:: because hermetic
// test archives only expose the mangled internal variant; the extern "C"
// public alias is not pulled in.
//
//===----------------------------------------------------------------------===//

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/fcntl/open.h"
#include "src/stdio/fprintf.h"
#include "src/stdio/stderr.h"
#include "src/sys/ioctl/ioctl.h"
#include "src/termios/tcgetattr.h"
#include "src/unistd/close.h"
#include "src/unistd/isatty.h"
#include "src/unistd/read.h"

namespace {

constexpr int CONTROL_FD = 3;

int fail(int stage) {
  LIBC_NAMESPACE::fprintf(LIBC_NAMESPACE::stderr,
                          "vt_pty_tree_join_child: stage=%d errno=%d\n", stage,
                          static_cast<int>(LIBC_NAMESPACE::libc_errno));
  return stage;
}

} // namespace

extern "C" int main() {
  // Read slave path from control fd (written by parent test).
  char path[128] = {};
  size_t offset = 0;
  while (offset + 1 < sizeof(path)) {
    ssize_t rc =
        LIBC_NAMESPACE::read(CONTROL_FD, path + offset,
                             sizeof(path) - offset - 1);
    if (rc < 0)
      return fail(1);
    if (rc == 0)
      break;
    offset += static_cast<size_t>(rc);
  }
  if (offset == 0)
    return fail(2);
  path[offset] = '\0';

  // Open the slave PTY.
  int fd = LIBC_NAMESPACE::open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0)
    return fail(3);

  // Validate: must be a terminal.
  if (LIBC_NAMESPACE::isatty(fd) != 1) {
    LIBC_NAMESPACE::close(fd);
    return fail(4);
  }

  // Validate: tcgetattr must succeed.
  struct termios attrs = {};
  if (LIBC_NAMESPACE::tcgetattr(fd, &attrs) != 0) {
    LIBC_NAMESPACE::close(fd);
    return fail(5);
  }

  // Validate: TIOCGWINSZ must succeed.
  struct winsize ws = {};
  if (LIBC_NAMESPACE::ioctl(fd, TIOCGWINSZ, &ws) != 0) {
    LIBC_NAMESPACE::close(fd);
    return fail(6);
  }

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::close(CONTROL_FD);
  return 0;
}
