//===-- PTY lifecycle: child helper ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Standalone child process for pty_lifecycle_test. Reads a PTY slave path
// from fd 3, opens it, validates terminal properties, and exercises basic I/O.
// Exit code 0 = success, non-zero = failure stage.
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr int CONTROL_FD = 3;

int fail(int stage) {
  fprintf(stderr, "pty_lifecycle_child: stage=%d errno=%d\n", stage, errno);
  return stage;
}

} // namespace

extern "C" int main() {
  // Read slave path from control fd.
  char path[128] = {};
  size_t offset = 0;
  while (offset + 1 < sizeof(path)) {
    ssize_t rc = read(CONTROL_FD, path + offset, sizeof(path) - offset - 1);
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
  int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0)
    return fail(3);

  // Must be a terminal.
  if (isatty(fd) != 1) {
    close(fd);
    return fail(4);
  }

  // tcgetattr must succeed.
  struct termios attrs = {};
  if (tcgetattr(fd, &attrs) != 0) {
    close(fd);
    return fail(5);
  }

  // TIOCGWINSZ must succeed.
  struct winsize ws = {};
  if (ioctl(fd, TIOCGWINSZ, &ws) != 0) {
    close(fd);
    return fail(6);
  }

  // Verify we can write to the terminal.
  const char msg[] = "child ok";
  if (write(fd, msg, sizeof(msg) - 1) < 0) {
    close(fd);
    return fail(7);
  }

  close(fd);
  close(CONTROL_FD);
  return 0;
}
