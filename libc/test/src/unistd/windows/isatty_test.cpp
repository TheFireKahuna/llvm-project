//===-- Windows unittests for isatty --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - isatty on a regular file returns 0 and sets ENOTTY
//   - isatty on a bad fd returns 0 and sets EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/isatty.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LlvmLibcWindowsIsattyTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// A regular file is not a terminal — isatty must return 0 and set ENOTTY.
TEST_F(LlvmLibcWindowsIsattyTest, RegularFile) {
  constexpr const char *PATH = "isatty.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  EXPECT_EQ(LIBC_NAMESPACE::isatty(fd), 0);
  ASSERT_ERRNO_EQ(ENOTTY);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// Bad fd: isatty must return 0 and set EBADF.
TEST_F(LlvmLibcWindowsIsattyTest, BadFd) {
  EXPECT_EQ(LIBC_NAMESPACE::isatty(-1), 0);
  ASSERT_ERRNO_EQ(EBADF);
}
