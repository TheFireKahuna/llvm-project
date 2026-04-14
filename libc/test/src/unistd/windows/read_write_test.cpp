//===-- Windows unittests for read and write ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - write then read roundtrip returns the same bytes
//   - write returns the number of bytes written
//   - read returns 0 at EOF
//   - write/read on a bad fd return EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/read.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsReadWriteTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcWindowsReadWriteTest, WriteAndReadBack) {
  constexpr const char *PATH = "read_write.tmp";
  constexpr char DATA[] = "hello windows";
  constexpr ssize_t LEN = sizeof(DATA); // include NUL

  int wfd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(wfd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::write(wfd, DATA, LEN), Succeeds(LEN));
  ASSERT_THAT(LIBC_NAMESPACE::close(wfd), Succeeds(0));

  int rfd = LIBC_NAMESPACE::open(PATH, O_RDONLY);
  ASSERT_GT(rfd, 0);
  char buf[sizeof(DATA)] = {};
  ASSERT_THAT(LIBC_NAMESPACE::read(rfd, buf, LEN), Succeeds(LEN));
  EXPECT_EQ(__builtin_memcmp(buf, DATA, LEN), 0);

  // A second read at EOF must return 0.
  EXPECT_THAT(LIBC_NAMESPACE::read(rfd, buf, LEN), Succeeds(ssize_t(0)));
  ASSERT_THAT(LIBC_NAMESPACE::close(rfd), Succeeds(0));

  LIBC_NAMESPACE::remove(PATH);
}

TEST_F(LlvmLibcWindowsReadWriteTest, WriteFailsBadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::write(-1, "x", 1), Fails<ssize_t>(EBADF));
}

TEST_F(LlvmLibcWindowsReadWriteTest, ReadFailsBadFd) {
  char buf[4];
  EXPECT_THAT(LIBC_NAMESPACE::read(-1, buf, sizeof(buf)), Fails<ssize_t>(EBADF));
}
