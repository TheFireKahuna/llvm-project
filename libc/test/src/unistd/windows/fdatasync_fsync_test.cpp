//===-- Windows unittests for fdatasync/fsync -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - fdatasync on a valid disk fd returns 0
//   - fsync on a valid disk fd returns 0
//   - both functions return EBADF on an invalid fd
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/fdatasync.h"
#include "src/unistd/fsync.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsFdatasyncFsyncTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// fdatasync on a disk fd with pending data must return 0.
TEST_F(LlvmLibcWindowsFdatasyncFsyncTest, FdatasyncSmoke) {
  constexpr const char *PATH = "fdatasync_test.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  LIBC_NAMESPACE::write(fd, "data", 4);
  EXPECT_THAT(LIBC_NAMESPACE::fdatasync(fd), Succeeds(0));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// fsync on a disk fd must return 0.
TEST_F(LlvmLibcWindowsFdatasyncFsyncTest, FsyncSmoke) {
  constexpr const char *PATH = "fsync_test.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  LIBC_NAMESPACE::write(fd, "data", 4);
  EXPECT_THAT(LIBC_NAMESPACE::fsync(fd), Succeeds(0));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// fdatasync/fsync on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsFdatasyncFsyncTest, BadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::fdatasync(-1), Fails(EBADF));
  EXPECT_THAT(LIBC_NAMESPACE::fsync(-1), Fails(EBADF));
}
