//===-- Windows unittests for ftruncate -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - ftruncate shrinks a file to the specified length
//   - ftruncate extends a file (zero-fills) to the specified length
//   - ftruncate on a bad fd returns EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/sys/stat/fstat.h"
#include "src/unistd/close.h"
#include "src/unistd/ftruncate.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include "hdr/types/off_t.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsFtruncateTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ftruncate to a smaller size: st_size must reflect the new length.
TEST_F(LlvmLibcWindowsFtruncateTest, Shrink) {
  constexpr const char *PATH = "ftruncate_shrink.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  constexpr char DATA[] = "0123456789";
  LIBC_NAMESPACE::write(fd, DATA, sizeof(DATA) - 1);

  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, 5), Succeeds(0));

  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd, &buf), Succeeds(0));
  EXPECT_EQ(buf.st_size, off_t(5));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// ftruncate to a larger size must zero-extend; st_size must match.
TEST_F(LlvmLibcWindowsFtruncateTest, Extend) {
  constexpr const char *PATH = "ftruncate_extend.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, 100), Succeeds(0));

  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd, &buf), Succeeds(0));
  EXPECT_EQ(buf.st_size, off_t(100));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// ftruncate on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsFtruncateTest, BadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::ftruncate(-1, 0), Fails(EBADF));
}
