//===-- Tree-scoped PTY join test (LLVM libc test framework) --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests that a child process can independently open and use a PTY slave
// allocated by the parent. The child is a separate executable
// (vt_pty_tree_join_child) that receives the slave path via a pipe on fd 3.
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"
#include "src/fcntl/posix_openpt.h"
#include "src/signal/signal.h"
#include "src/spawn/posix_spawn.h"
#include "src/spawn/posix_spawn_file_actions_addclose.h"
#include "src/spawn/posix_spawn_file_actions_adddup2.h"
#include "src/spawn/posix_spawn_file_actions_destroy.h"
#include "src/spawn/posix_spawn_file_actions_init.h"
#include "src/stdlib/grantpt.h"
#include "src/stdlib/ptsname_r.h"
#include "src/stdlib/unlockpt.h"
#include "src/string/strlen.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/close.h"
#include "src/unistd/pipe.h"
#include "src/unistd/write.h"
#include "test/UnitTest/Test.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

// Path to the child helper binary. Set via CMake to be co-located with the
// test executable in the same output directory.
#ifndef VT_PTY_TREE_JOIN_CHILD_PATH
#define VT_PTY_TREE_JOIN_CHILD_PATH "vt_pty_tree_join_child"
#endif

constexpr int CHILD_CONTROL_FD = 3;

} // namespace

TEST(LlvmLibcVtPtyTreeJoinTest, ChildCanOpenParentAllocatedSlave) {
  LIBC_NAMESPACE::signal(SIGPIPE, SIG_IGN);

  // Create a pipe to pass the slave path to the child.
  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(LIBC_NAMESPACE::pipe(pipe_fds), 0);

  // Set up posix_spawn file actions: dup pipe read end to fd 3.
  posix_spawn_file_actions_t actions = {};
  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_init(&actions), 0);
  ASSERT_EQ(
      LIBC_NAMESPACE::posix_spawn_file_actions_adddup2(&actions, pipe_fds[0],
                                                       CHILD_CONTROL_FD),
      0);
  ASSERT_EQ(
      LIBC_NAMESPACE::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]),
      0);
  if (pipe_fds[0] != CHILD_CONTROL_FD)
    ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_addclose(&actions,
                                                                pipe_fds[0]),
              0);

  // Spawn the child helper.
  const char *child_path = VT_PTY_TREE_JOIN_CHILD_PATH;
  char *child_argv[] = {const_cast<char *>(child_path), nullptr};
  pid_t child_pid = 0;
  int spawn_rc =
      LIBC_NAMESPACE::posix_spawn(&child_pid, child_path, &actions, nullptr,
                                  child_argv, environ);
  LIBC_NAMESPACE::posix_spawn_file_actions_destroy(&actions);
  ASSERT_EQ(spawn_rc, 0);
  LIBC_NAMESPACE::close(pipe_fds[0]);

  // Allocate a master PTY and prepare the slave.
  int ptmx_fd = LIBC_NAMESPACE::posix_openpt(O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(LIBC_NAMESPACE::grantpt(ptmx_fd), 0);
  ASSERT_EQ(LIBC_NAMESPACE::unlockpt(ptmx_fd), 0);

  char slave_name[64] = {};
  ASSERT_EQ(
      LIBC_NAMESPACE::ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);

  // Send the slave path to the child via the pipe.
  size_t name_len = LIBC_NAMESPACE::strlen(slave_name);
  ssize_t written = LIBC_NAMESPACE::write(pipe_fds[1], slave_name, name_len);
  LIBC_NAMESPACE::close(pipe_fds[1]);
  ASSERT_EQ(written, static_cast<ssize_t>(name_len));

  // Wait for the child to complete.
  int status = 0;
  ASSERT_GT(LIBC_NAMESPACE::waitpid(child_pid, &status, 0),
            static_cast<pid_t>(0));
  LIBC_NAMESPACE::close(ptmx_fd);

  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(LlvmLibcVtPtyTreeJoinTest, ChildFailsOnInvalidSlavePath) {
  LIBC_NAMESPACE::signal(SIGPIPE, SIG_IGN);

  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(LIBC_NAMESPACE::pipe(pipe_fds), 0);

  posix_spawn_file_actions_t actions = {};
  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_init(&actions), 0);
  ASSERT_EQ(
      LIBC_NAMESPACE::posix_spawn_file_actions_adddup2(&actions, pipe_fds[0],
                                                       CHILD_CONTROL_FD),
      0);
  ASSERT_EQ(
      LIBC_NAMESPACE::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]),
      0);
  if (pipe_fds[0] != CHILD_CONTROL_FD)
    ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_addclose(&actions,
                                                                pipe_fds[0]),
              0);

  const char *child_path = VT_PTY_TREE_JOIN_CHILD_PATH;
  char *child_argv[] = {const_cast<char *>(child_path), nullptr};
  pid_t child_pid = 0;
  int spawn_rc =
      LIBC_NAMESPACE::posix_spawn(&child_pid, child_path, &actions, nullptr,
                                  child_argv, environ);
  LIBC_NAMESPACE::posix_spawn_file_actions_destroy(&actions);
  ASSERT_EQ(spawn_rc, 0);
  LIBC_NAMESPACE::close(pipe_fds[0]);

  // Send a bogus path — child should fail at open().
  const char *bogus = "/dev/pts/999999";
  size_t bogus_len = LIBC_NAMESPACE::strlen(bogus);
  ssize_t written = LIBC_NAMESPACE::write(pipe_fds[1], bogus, bogus_len);
  LIBC_NAMESPACE::close(pipe_fds[1]);
  ASSERT_EQ(written, static_cast<ssize_t>(bogus_len));

  int status = 0;
  ASSERT_GT(LIBC_NAMESPACE::waitpid(child_pid, &status, 0),
            static_cast<pid_t>(0));

  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_NE(WEXITSTATUS(status), 0);
}
