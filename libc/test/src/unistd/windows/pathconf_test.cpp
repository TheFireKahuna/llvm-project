//===-- Windows unittests for pathconf/fpathconf --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   pathconf:
//   - _PC_NAME_MAX is positive for an existing path (255 on NTFS)
//   - _PC_PATH_MAX is positive (32767 on NT)
//   - nonexistent path returns ENOENT
//   - unknown name returns EINVAL
//   fpathconf:
//   - same results as pathconf for an open fd
//   - bad fd returns EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/fpathconf.h"
#include "src/unistd/pathconf.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include "hdr/unistd_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LlvmLibcWindowsPathconfTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// pathconf _PC_NAME_MAX must be positive (NTFS returns 255).
TEST_F(LlvmLibcWindowsPathconfTest, NameMax) {
  constexpr const char *PATH = "pathconf_test.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  long val = LIBC_NAMESPACE::pathconf(PATH, _PC_NAME_MAX);
  EXPECT_GT(val, 0L);

  LIBC_NAMESPACE::remove(PATH);
}

// pathconf _PC_PATH_MAX must be positive (NT returns 32767).
TEST_F(LlvmLibcWindowsPathconfTest, PathMax) {
  constexpr const char *PATH = "pathconf_pm.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  long val = LIBC_NAMESPACE::pathconf(PATH, _PC_PATH_MAX);
  EXPECT_GT(val, 0L);

  LIBC_NAMESPACE::remove(PATH);
}

// pathconf on a nonexistent path must set ENOENT and return -1.
TEST_F(LlvmLibcWindowsPathconfTest, NonexistentPath) {
  EXPECT_THAT(
      LIBC_NAMESPACE::pathconf("__no_such_file_xyz__.tmp", _PC_NAME_MAX),
      Fails<long>(ENOENT));
}

// pathconf with an unknown name must return EINVAL.
TEST_F(LlvmLibcWindowsPathconfTest, UnknownName) {
  constexpr const char *PATH = "pathconf_unk.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::pathconf(PATH, 99999), Fails<long>(EINVAL));
  LIBC_NAMESPACE::remove(PATH);
}

// fpathconf must return the same _PC_NAME_MAX for an open fd.
TEST_F(LlvmLibcWindowsPathconfTest, FpathconfNameMax) {
  constexpr const char *PATH = "fpathconf_test.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  long val = LIBC_NAMESPACE::fpathconf(fd, _PC_NAME_MAX);
  EXPECT_GT(val, 0L);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// fpathconf on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsPathconfTest, FpathconfBadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::fpathconf(-1, _PC_NAME_MAX), Fails<long>(EBADF));
}
