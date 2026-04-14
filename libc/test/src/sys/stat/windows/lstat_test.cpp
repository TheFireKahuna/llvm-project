//===-- Windows unittests for lstat ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - lstat on an existing regular file succeeds and reports S_IFREG
//   - lstat on a nonexistent path returns ENOENT
//
// Note: Windows does not distinguish stat/lstat for regular files. Symlink
// semantics are tested only if the platform creates a true symlink.
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/lstat.h"
#include "src/unistd/close.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsLStatTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// lstat on a regular file must succeed and report S_IFREG.
TEST_F(LlvmLibcWindowsLStatTest, ExistingFile) {
  constexpr const char *PATH = "lstat_existing.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::lstat(PATH, &buf), Succeeds(0));
  EXPECT_TRUE((buf.st_mode & S_IFMT) == S_IFREG);

  LIBC_NAMESPACE::unlink(PATH);
}

// lstat on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsLStatTest, NonexistentPath) {
  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::lstat("no_such_lstat_xyz.tmp", &buf),
              Fails(ENOENT));
}
