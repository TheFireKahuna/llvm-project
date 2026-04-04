//===-- Windows unittests for access --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - access(F_OK) on an existing file succeeds
//   - access(F_OK) on a nonexistent file returns ENOENT
//   - access(R_OK|W_OK) on a file we created succeeds
//   - access on a directory with F_OK succeeds
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/sys/stat/mkdir.h"
#include "src/unistd/access.h"
#include "src/unistd/close.h"
#include "src/unistd/rmdir.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsAccessTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// access(F_OK) on an existing file must succeed.
TEST_F(LlvmLibcWindowsAccessTest, ExistingFile) {
  constexpr const char *PATH = "access_existing.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::access(PATH, F_OK), Succeeds(0));
  LIBC_NAMESPACE::remove(PATH);
}

// access(F_OK) on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsAccessTest, NonexistentFile) {
  EXPECT_THAT(LIBC_NAMESPACE::access("no_such_access.tmp", F_OK), Fails(ENOENT));
}

// access(R_OK|W_OK) on a file we created with IRWXU must succeed.
TEST_F(LlvmLibcWindowsAccessTest, ReadWritePermission) {
  constexpr const char *PATH = "access_rwx.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::access(PATH, R_OK | W_OK), Succeeds(0));
  LIBC_NAMESPACE::remove(PATH);
}

// access(F_OK) on an existing directory must succeed.
TEST_F(LlvmLibcWindowsAccessTest, Directory) {
  constexpr const char *DIR = "access_dir.tmp";
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::access(DIR, F_OK), Succeeds(0));
  LIBC_NAMESPACE::rmdir(DIR);
}
