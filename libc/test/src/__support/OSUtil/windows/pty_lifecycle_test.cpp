//===-- PTY lifecycle integration test ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Integration test for the full POSIX PTY lifecycle:
//   posix_openpt → grantpt → unlockpt → ptsname_r → fork/exec →
//   setsid → open slave → session establishment → I/O → teardown
//
// Uses posix_spawn (NTPOSIX equivalent of fork/exec) with a child helper
// that opens the slave, establishes a session, and verifies terminal I/O.
//
//===----------------------------------------------------------------------===//

#include "test/UnitTest/Test.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

namespace {

#ifndef PTY_LIFECYCLE_CHILD_PATH
#define PTY_LIFECYCLE_CHILD_PATH "pty_lifecycle_child"
#endif

constexpr int CHILD_CONTROL_FD = 3;

} // namespace

// ===----------------------------------------------------------------------===
// Full lifecycle: allocate → prepare → spawn child → child opens slave → I/O
// ===----------------------------------------------------------------------===

TEST(LlvmLibcPtyLifecycleTest, FullAllocatePreparePtsnameOpenLifecycle) {
  // Step 1: posix_openpt — allocate a master PTY.
  int ptmx_fd = posix_openpt(O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);

  // Step 2: grantpt — adjust slave permissions.
  ASSERT_EQ(grantpt(ptmx_fd), 0);

  // Step 3: unlockpt — unlock the slave.
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  // Step 4: ptsname_r — get slave device path.
  char slave_name[64] = {};
  ASSERT_EQ(ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);
  ASSERT_GT(strlen(slave_name), static_cast<size_t>(0));

  // Step 5: Open slave from the parent (no session establishment).
  int slave_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  ASSERT_GE(slave_fd, 0);

  // Verify the slave is a terminal.
  EXPECT_EQ(isatty(slave_fd), 1);

  // Verify terminal attributes are readable.
  struct termios attrs = {};
  EXPECT_EQ(tcgetattr(slave_fd, &attrs), 0);

  // Verify window size is queryable.
  struct winsize ws = {};
  EXPECT_EQ(ioctl(slave_fd, TIOCGWINSZ, &ws), 0);

  close(slave_fd);
  close(ptmx_fd);
}

TEST(LlvmLibcPtyLifecycleTest, MasterSlaveIOPassthrough) {
  int master_fd = -1, slave_fd = -1;
  ASSERT_EQ(openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr), 0);
  ASSERT_GE(master_fd, 0);
  ASSERT_GE(slave_fd, 0);

  // Put slave in raw mode for clean I/O testing.
  struct termios t = {};
  tcgetattr(slave_fd, &t);
  t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
  t.c_iflag &= ~static_cast<tcflag_t>(ICRNL | INLCR | IGNCR | IXON | IXOFF);
  t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(slave_fd, TCSANOW, &t);

  // Master → Slave: write to master, read from slave.
  const char *msg = "hello from master";
  size_t msg_len = strlen(msg);
  ASSERT_EQ(write(master_fd, msg, msg_len), static_cast<ssize_t>(msg_len));

  char buf[64] = {};
  // Set non-blocking with a small retry to handle async delivery.
  int flags = fcntl(slave_fd, F_GETFL);
  fcntl(slave_fd, F_SETFL, flags | O_NONBLOCK);

  ssize_t total = 0;
  for (int i = 0; i < 50 && total < static_cast<ssize_t>(msg_len); ++i) {
    ssize_t n = read(slave_fd, buf + total,
                     sizeof(buf) - static_cast<size_t>(total));
    if (n > 0)
      total += n;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      usleep(1000);
    else
      break;
  }
  fcntl(slave_fd, F_SETFL, flags);

  ASSERT_EQ(total, static_cast<ssize_t>(msg_len));
  EXPECT_EQ(memcmp(buf, msg, msg_len), 0);

  // Slave → Master: write to slave (raw, no OPOST), read from master.
  const char *reply = "reply from slave";
  size_t reply_len = strlen(reply);
  ASSERT_EQ(write(slave_fd, reply, reply_len),
            static_cast<ssize_t>(reply_len));

  char rbuf[64] = {};
  flags = fcntl(master_fd, F_GETFL);
  fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

  total = 0;
  for (int i = 0; i < 50 && total < static_cast<ssize_t>(reply_len); ++i) {
    ssize_t n = read(master_fd, rbuf + total,
                     sizeof(rbuf) - static_cast<size_t>(total));
    if (n > 0)
      total += n;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      usleep(1000);
    else
      break;
  }
  fcntl(master_fd, F_SETFL, flags);

  ASSERT_EQ(total, static_cast<ssize_t>(reply_len));
  EXPECT_EQ(memcmp(rbuf, reply, reply_len), 0);

  close(slave_fd);
  close(master_fd);
}

TEST(LlvmLibcPtyLifecycleTest, OpenptyPreservesTermiosAndWinsize) {
  struct termios custom_attrs = {};
  custom_attrs.c_lflag = ICANON | ECHO;
  custom_attrs.c_iflag = ICRNL;
  custom_attrs.c_oflag = OPOST | ONLCR;
  custom_attrs.c_cc[VMIN] = 1;
  custom_attrs.c_cc[VEOF] = 0x04; // Ctrl-D

  struct winsize custom_ws = {};
  custom_ws.ws_row = 40;
  custom_ws.ws_col = 120;

  int master_fd = -1, slave_fd = -1;
  int rc = openpty(&master_fd, &slave_fd, nullptr, &custom_attrs, &custom_ws);
  ASSERT_EQ(rc, 0);

  // Verify the termios were applied.
  struct termios actual_attrs = {};
  ASSERT_EQ(tcgetattr(slave_fd, &actual_attrs), 0);
  EXPECT_EQ(actual_attrs.c_lflag & (ICANON | ECHO),
            static_cast<tcflag_t>(ICANON | ECHO));
  EXPECT_EQ(actual_attrs.c_cc[VEOF], static_cast<cc_t>(0x04));

  // Verify the winsize was applied.
  struct winsize actual_ws = {};
  ASSERT_EQ(ioctl(slave_fd, TIOCGWINSZ, &actual_ws), 0);
  EXPECT_EQ(actual_ws.ws_row, static_cast<unsigned short>(40));
  EXPECT_EQ(actual_ws.ws_col, static_cast<unsigned short>(120));

  close(slave_fd);
  close(master_fd);
}

TEST(LlvmLibcPtyLifecycleTest, ChildProcessCanOpenAndUseSlave) {
  signal(SIGPIPE, SIG_IGN);

  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(pipe(pipe_fds), 0);

  // Spawn child that receives the slave path on fd 3.
  posix_spawn_file_actions_t actions = {};
  ASSERT_EQ(posix_spawn_file_actions_init(&actions), 0);
  ASSERT_EQ(
      posix_spawn_file_actions_adddup2(&actions, pipe_fds[0], CHILD_CONTROL_FD),
      0);
  ASSERT_EQ(posix_spawn_file_actions_addclose(&actions, pipe_fds[1]), 0);
  if (pipe_fds[0] != CHILD_CONTROL_FD)
    ASSERT_EQ(posix_spawn_file_actions_addclose(&actions, pipe_fds[0]), 0);

  const char *child_path = PTY_LIFECYCLE_CHILD_PATH;
  char *child_argv[] = {const_cast<char *>(child_path), nullptr};
  pid_t child_pid = 0;
  int spawn_rc =
      posix_spawn(&child_pid, child_path, &actions, nullptr, child_argv,
                  environ);
  posix_spawn_file_actions_destroy(&actions);
  ASSERT_EQ(spawn_rc, 0);
  close(pipe_fds[0]);

  // Allocate PTY and send slave path.
  int ptmx_fd = posix_openpt(O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  char slave_name[64] = {};
  ASSERT_EQ(ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);

  size_t name_len = strlen(slave_name);
  ssize_t written = write(pipe_fds[1], slave_name, name_len);
  close(pipe_fds[1]);
  ASSERT_EQ(written, static_cast<ssize_t>(name_len));

  int status = 0;
  ASSERT_GT(waitpid(child_pid, &status, 0), static_cast<pid_t>(0));
  close(ptmx_fd);

  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(LlvmLibcPtyLifecycleTest, MultipleSlavesFromSameMaster) {
  int ptmx_fd = posix_openpt(O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  char slave_name[64] = {};
  ASSERT_EQ(ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);

  // Open the slave twice — both should succeed.
  int slave1 = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  ASSERT_GE(slave1, 0);

  int slave2 = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  ASSERT_GE(slave2, 0);

  EXPECT_NE(slave1, slave2);
  EXPECT_EQ(isatty(slave1), 1);
  EXPECT_EQ(isatty(slave2), 1);

  close(slave2);
  close(slave1);
  close(ptmx_fd);
}

TEST(LlvmLibcPtyLifecycleTest, ClosingMasterSignalsSlaveEof) {
  int master_fd = -1, slave_fd = -1;
  ASSERT_EQ(openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr), 0);

  // Put slave in raw mode.
  struct termios t = {};
  tcgetattr(slave_fd, &t);
  t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
  t.c_iflag &= ~static_cast<tcflag_t>(ICRNL | IXON);
  t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 1; // 100ms timeout
  tcsetattr(slave_fd, TCSANOW, &t);

  // Close master — slave reads should now return 0 (EOF) or EIO.
  close(master_fd);
  master_fd = -1;

  char buf[16] = {};
  ssize_t n = read(slave_fd, buf, sizeof(buf));
  // After master close: 0 (EOF) or -1 with EIO are both valid responses.
  EXPECT_TRUE(n == 0 || (n == -1 && errno == EIO));

  close(slave_fd);
}
