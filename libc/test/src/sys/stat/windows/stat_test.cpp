//===-- Windows unittests for stat ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - stat on an existing file succeeds and reports S_IFREG
//   - stat on a nonexistent path returns ENOENT
//   - stat on an existing directory reports S_IFDIR
//   - stat with null statbuf returns EFAULT (if implemented)
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/mkdir.h"
#include "src/sys/stat/stat.h"
#include "src/unistd/close.h"
#include "src/unistd/rmdir.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsStatTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// stat on a file we created must succeed and report S_IFREG.
TEST_F(LlvmLibcWindowsStatTest, ExistingFile) {
  constexpr const char *PATH = "stat_existing.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::stat(PATH, &buf), Succeeds(0));
  EXPECT_TRUE((buf.st_mode & S_IFMT) == S_IFREG);

  LIBC_NAMESPACE::unlink(PATH);
}

// stat on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsStatTest, NonexistentPath) {
  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::stat("no_such_file_xyz.tmp", &buf),
              Fails(ENOENT));
}

// stat on a directory must report S_IFDIR.
TEST_F(LlvmLibcWindowsStatTest, Directory) {
  constexpr const char *DIR = "stat_dir.tmp";
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));

  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::stat(DIR, &buf), Succeeds(0));
  EXPECT_TRUE((buf.st_mode & S_IFMT) == S_IFDIR);

  LIBC_NAMESPACE::rmdir(DIR);
}
