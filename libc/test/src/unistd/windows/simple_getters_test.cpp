//===-- Windows unittests for gettid/getppid/gethostname/getpagesize ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Smoke tests for simple query functions:
//   - gettid: positive, stable across two calls within a thread
//   - getppid: positive, different from our own pid
//   - gethostname: returns 0, non-empty string; ENAMETOOLONG on tiny buffer
//   - getpagesize: positive, power of 2
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getpagesize.h"
#include "src/unistd/getpid.h"
#include "src/unistd/getppid.h"
#include "src/unistd/gethostname.h"
#include "src/unistd/gettid.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include "hdr/types/pid_t.h"

using LlvmLibcWindowsSimpleGettersTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// gettid must be positive and consistent within a single thread.
TEST_F(LlvmLibcWindowsSimpleGettersTest, Gettid) {
  pid_t t1 = LIBC_NAMESPACE::gettid();
  pid_t t2 = LIBC_NAMESPACE::gettid();
  EXPECT_GT(t1, pid_t(0));
  EXPECT_EQ(t1, t2);
}

// getppid must be positive and differ from our own pid.
TEST_F(LlvmLibcWindowsSimpleGettersTest, Getppid) {
  pid_t ppid = LIBC_NAMESPACE::getppid();
  pid_t pid  = LIBC_NAMESPACE::getpid();
  EXPECT_GT(ppid, pid_t(0));
  EXPECT_NE(ppid, pid);
}

// gethostname must return 0 and fill the buffer with a non-empty string.
TEST_F(LlvmLibcWindowsSimpleGettersTest, Gethostname) {
  char buf[256] = {};
  int ret = LIBC_NAMESPACE::gethostname(buf, sizeof(buf));
  EXPECT_EQ(ret, 0);
  EXPECT_NE(buf[0], '\0');
}

// gethostname with a 1-byte buffer must fail with ENAMETOOLONG.
TEST_F(LlvmLibcWindowsSimpleGettersTest, GethostnameTooSmall) {
  char tiny[1] = {};
  int ret = LIBC_NAMESPACE::gethostname(tiny, sizeof(tiny));
  EXPECT_EQ(ret, -1);
  ASSERT_ERRNO_EQ(ENAMETOOLONG);
}

// getpagesize must be positive and a power of 2.
TEST_F(LlvmLibcWindowsSimpleGettersTest, Getpagesize) {
  int ps = LIBC_NAMESPACE::getpagesize();
  EXPECT_GT(ps, 0);
  // A power of 2 satisfies (ps & (ps-1)) == 0.
  EXPECT_EQ(ps & (ps - 1), 0);
}
