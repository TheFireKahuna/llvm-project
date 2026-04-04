//===-- Integration tests for waitid --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test_binary_properties.h"

#include "src/spawn/posix_spawn.h"
#include "src/spawn/posix_spawn_file_actions_addopen.h"
#include "src/spawn/posix_spawn_file_actions_destroy.h"
#include "src/spawn/posix_spawn_file_actions_init.h"
#include "src/sys/wait/waitid.h"
#include "test/IntegrationTest/test.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

char waitid_arg0[] = "libc_posix_spawn_test_binary";
char *waitid_argv[] = {
    waitid_arg0,
    nullptr,
};

TEST_MAIN([[maybe_unused]] int argc, [[maybe_unused]] char **argv, char **envp) {
  posix_spawn_file_actions_t file_actions;
  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_init(&file_actions), 0);
  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_addopen(
                &file_actions, CHILD_FD, "testdata/posix_spawn.test", O_RDONLY,
                0),
            0);

  pid_t child = 0;
  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn(&child, waitid_arg0, &file_actions,
                                        nullptr, waitid_argv, envp),
            0);
  ASSERT_TRUE(child > 0);

  siginfo_t info = {};
  ASSERT_EQ(LIBC_NAMESPACE::waitid(P_PID, static_cast<id_t>(child), &info,
                                   WEXITED | WNOWAIT),
            0);
  ASSERT_EQ(info.si_signo, SIGCHLD);
  ASSERT_EQ(info.si_pid, child);
  ASSERT_EQ(info.si_code, CLD_EXITED);
  ASSERT_EQ(info.si_status, 0);

  siginfo_t reaped = {};
  ASSERT_EQ(LIBC_NAMESPACE::waitid(P_PID, static_cast<id_t>(child), &reaped,
                                   WEXITED),
            0);
  ASSERT_EQ(reaped.si_signo, SIGCHLD);
  ASSERT_EQ(reaped.si_pid, child);
  ASSERT_EQ(reaped.si_code, CLD_EXITED);
  ASSERT_EQ(reaped.si_status, 0);

  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn_file_actions_destroy(&file_actions), 0);
  return 0;
}
