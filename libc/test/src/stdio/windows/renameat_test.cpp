//===-- Windows unittests for renameat ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - renameat with AT_FDCWD behaves identically to rename
//   - renameat atomically replaces an existing destination
//   - renameat on a nonexistent source returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/renameat.h"
#include "src/sys/stat/stat.h"
#include "src/unistd/close.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h> // AT_FDCWD

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsRenameatTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// renameat with AT_FDCWD moves src to dst; src must no longer exist.
TEST_F(LlvmLibcWindowsRenameatTest, AtFdcwdRenames) {
  constexpr const char *SRC = "renameat_src.tmp";
  constexpr const char *DST = "renameat_dst.tmp";

  int fd = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  ASSERT_THAT(LIBC_NAMESPACE::renameat(AT_FDCWD, SRC, AT_FDCWD, DST),
              Succeeds(0));

  // DST must exist; SRC must not.
  struct stat st;
  EXPECT_THAT(LIBC_NAMESPACE::stat(DST, &st), Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::stat(SRC, &st), Fails(ENOENT));

  LIBC_NAMESPACE::unlink(DST);
}

// renameat atomically replaces an existing destination.
TEST_F(LlvmLibcWindowsRenameatTest, ReplacesExisting) {
  constexpr const char *SRC = "renameat_rep_src.tmp";
  constexpr const char *DST = "renameat_rep_dst.tmp";

  int fd1 = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd1, 0);
  LIBC_NAMESPACE::close(fd1);

  int fd2 = LIBC_NAMESPACE::open(DST, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd2, 0);
  LIBC_NAMESPACE::close(fd2);

  EXPECT_THAT(LIBC_NAMESPACE::renameat(AT_FDCWD, SRC, AT_FDCWD, DST),
              Succeeds(0));

  struct stat st;
  EXPECT_THAT(LIBC_NAMESPACE::stat(DST, &st), Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::stat(SRC, &st), Fails(ENOENT));

  LIBC_NAMESPACE::unlink(DST);
}

// renameat on a nonexistent source must return ENOENT.
TEST_F(LlvmLibcWindowsRenameatTest, NonexistentSource) {
  EXPECT_THAT(
      LIBC_NAMESPACE::renameat(AT_FDCWD, "__no_src_xyz__.tmp",
                               AT_FDCWD, "renameat_out.tmp"),
      Fails(ENOENT));
}
