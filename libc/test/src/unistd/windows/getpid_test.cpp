//===-- Windows unittests for getpid --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - getpid returns a positive PID
//   - Consecutive calls return the same PID (process ID is stable)
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getpid.h"
#include "test/UnitTest/Test.h"

#include "hdr/types/pid_t.h"

TEST(LlvmLibcWindowsGetpidTest, ReturnsPositive) {
  pid_t pid = LIBC_NAMESPACE::getpid();
  EXPECT_GT(pid, pid_t(0));
}

TEST(LlvmLibcWindowsGetpidTest, Stable) {
  pid_t p1 = LIBC_NAMESPACE::getpid();
  pid_t p2 = LIBC_NAMESPACE::getpid();
  EXPECT_EQ(p1, p2);
}
