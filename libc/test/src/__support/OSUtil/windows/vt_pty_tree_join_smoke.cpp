//===-- Tree-scoped PTY join smoke ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

namespace {

constexpr int CHILD_CONTROL_FD = 3;

int fail(int stage) {
  fprintf(stderr, "vt_pty_tree_join_smoke: stage=%d errno=%d\n", stage, errno);
  return stage;
}

int child_main() {
  fprintf(stderr, "child: start\n");
  char path[128] = {};
  size_t offset = 0;
  while (offset + 1 < sizeof(path)) {
    ssize_t rc = read(CHILD_CONTROL_FD, path + offset, sizeof(path) - offset - 1);
    if (rc < 0)
      return fail(110);
    if (rc == 0)
      break;
    offset += static_cast<size_t>(rc);
  }

  if (offset == 0)
    return fail(111);
  path[offset] = '\0';
  fprintf(stderr, "child: path=%s\n", path);

  int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0)
    return fail(112);
  fprintf(stderr, "child: open ok fd=%d\n", fd);

  if (isatty(fd) != 1) {
    close(fd);
    errno = ENOTTY;
    return fail(113);
  }

  struct termios attrs = {};
  if (tcgetattr(fd, &attrs) != 0) {
    close(fd);
    return fail(114);
  }

  struct winsize ws = {};
  if (ioctl(fd, TIOCGWINSZ, &ws) != 0) {
    close(fd);
    return fail(115);
  }

  close(fd);
  close(CHILD_CONTROL_FD);
  return 0;
}

} // namespace

extern "C" int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--child") == 0)
    return child_main();

  signal(SIGPIPE, SIG_IGN);

  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0)
    return fail(10);

  posix_spawn_file_actions_t actions = {};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return fail(11);
  }

  bool actions_ok = true;
  if (posix_spawn_file_actions_adddup2(&actions, pipe_fds[0], CHILD_CONTROL_FD) !=
      0)
    actions_ok = false;
  if (actions_ok &&
      posix_spawn_file_actions_addclose(&actions, pipe_fds[1]) != 0)
    actions_ok = false;
  if (actions_ok &&
      pipe_fds[0] != CHILD_CONTROL_FD &&
      posix_spawn_file_actions_addclose(&actions, pipe_fds[0]) != 0)
    actions_ok = false;
  if (!actions_ok) {
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return fail(12);
  }

  char *child_argv[] = {argv[0], const_cast<char *>("--child"), nullptr};
  pid_t child_pid = 0;
  int spawn_rc =
      posix_spawn(&child_pid, argv[0], &actions, nullptr, child_argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_rc != 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    errno = spawn_rc;
    return fail(13);
  }

  close(pipe_fds[0]);

  int ptmx_fd = posix_openpt(O_RDWR | O_CLOEXEC);
  if (ptmx_fd < 0) {
    close(pipe_fds[1]);
    return fail(14);
  }

  if (grantpt(ptmx_fd) != 0) {
    close(ptmx_fd);
    close(pipe_fds[1]);
    return fail(15);
  }

  if (unlockpt(ptmx_fd) != 0) {
    close(ptmx_fd);
    close(pipe_fds[1]);
    return fail(16);
  }

  char slave_name[64] = {};
  if (ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)) != 0) {
    close(ptmx_fd);
    close(pipe_fds[1]);
    return fail(17);
  }

  size_t name_len = strlen(slave_name);
  bool write_ok =
      write(pipe_fds[1], slave_name, name_len) == static_cast<ssize_t>(name_len);
  close(pipe_fds[1]);

  int status = 0;
  if (waitpid(child_pid, &status, 0) < 0) {
    close(ptmx_fd);
    return fail(19);
  }
  fprintf(stderr, "parent: raw wait status=0x%x exited=%d exitstatus=%d signaled=%d termsig=%d\n",
          status, WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status),
          WIFSIGNALED(status) ? WTERMSIG(status) : 0);

  close(ptmx_fd);
  if (!write_ok)
    return fail(18);
  if (!WIFEXITED(status))
    return fail(20);
  return WEXITSTATUS(status);
}
