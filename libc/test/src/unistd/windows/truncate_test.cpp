//===-- Windows unittests for truncate ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - truncate shrinks a file to the specified length
//   - truncate extends a file (zero-fills) to the specified length
//   - truncate on a nonexistent path returns ENOENT
//   - truncate with a negative length returns EINVAL
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/sys/stat/fstat.h"
#include "src/unistd/close.h"
#include "src/unistd/truncate.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include "hdr/types/off_t.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsTruncateTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// truncate to a smaller size; st_size must reflect the new length.
TEST_F(LlvmLibcWindowsTruncateTest, Shrink) {
  constexpr const char *PATH = "truncate_shrink.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  constexpr char DATA[] = "0123456789";
  LIBC_NAMESPACE::write(fd, DATA, sizeof(DATA) - 1);
  LIBC_NAMESPACE::close(fd);

  ASSERT_THAT(LIBC_NAMESPACE::truncate(PATH, 5), Succeeds(0));

  int fd2 = LIBC_NAMESPACE::open(PATH, O_RDONLY);
  ASSERT_GT(fd2, 0);
  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd2, &buf), Succeeds(0));
  EXPECT_EQ(buf.st_size, off_t(5));
  LIBC_NAMESPACE::close(fd2);
  LIBC_NAMESPACE::remove(PATH);
}

// truncate to a larger size must zero-extend; st_size must match.
TEST_F(LlvmLibcWindowsTruncateTest, Extend) {
  constexpr const char *PATH = "truncate_extend.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  ASSERT_THAT(LIBC_NAMESPACE::truncate(PATH, 100), Succeeds(0));

  int fd2 = LIBC_NAMESPACE::open(PATH, O_RDONLY);
  ASSERT_GT(fd2, 0);
  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd2, &buf), Succeeds(0));
  EXPECT_EQ(buf.st_size, off_t(100));
  LIBC_NAMESPACE::close(fd2);
  LIBC_NAMESPACE::remove(PATH);
}

// truncate on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsTruncateTest, NonexistentPath) {
  EXPECT_THAT(LIBC_NAMESPACE::truncate("__no_such_file_xyz__.tmp", 0),
              Fails(ENOENT));
}

// truncate with a negative length must return EINVAL.
TEST_F(LlvmLibcWindowsTruncateTest, NegativeLength) {
  EXPECT_THAT(LIBC_NAMESPACE::truncate("anything.tmp", -1), Fails(EINVAL));
}
