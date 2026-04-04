//===-- Windows unittests for dup3/fchdir ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// dup3:
//   - creates a copy at a specific fd number
//   - oldfd == newfd returns EINVAL (unlike dup2 which allows it)
//   - O_CLOEXEC sets FD_CLOEXEC on the new fd
//   - bad oldfd returns EBADF
//
// fchdir:
//   - changes the working directory via an open directory fd
//   - a subsequent getcwd reflects the change
//   - bad fd returns EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/fcntl.h"
#include "src/fcntl/open.h"
#include "src/sys/stat/mkdir.h"
#include "src/unistd/chdir.h"
#include "src/unistd/close.h"
#include "src/unistd/dup3.h"
#include "src/unistd/fchdir.h"
#include "src/unistd/getcwd.h"
#include "src/unistd/rmdir.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsDup3FchdirTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ── dup3 ─────────────────────────────────────────────────────────────────────

// dup3 must return exactly the requested target fd.
TEST_F(LlvmLibcWindowsDup3FchdirTest, Dup3ReturnsTarget) {
  constexpr const char *PATH = "dup3_target.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  constexpr int TARGET = 60;
  ASSERT_THAT(LIBC_NAMESPACE::dup3(fd, TARGET, 0), Succeeds(TARGET));
  EXPECT_NE(TARGET, fd);

  LIBC_NAMESPACE::close(TARGET);
  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// dup3 with oldfd == newfd must return EINVAL (unlike dup2).
TEST_F(LlvmLibcWindowsDup3FchdirTest, Dup3SameFdIsEinval) {
  constexpr const char *PATH = "dup3_same.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  EXPECT_THAT(LIBC_NAMESPACE::dup3(fd, fd, 0), Fails(EINVAL));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// dup3 with O_CLOEXEC must set FD_CLOEXEC on the new fd.
TEST_F(LlvmLibcWindowsDup3FchdirTest, Dup3Cloexec) {
  constexpr const char *PATH = "dup3_cloexec.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  constexpr int TARGET = 61;
  ASSERT_THAT(LIBC_NAMESPACE::dup3(fd, TARGET, O_CLOEXEC), Succeeds(TARGET));

  int flags = LIBC_NAMESPACE::fcntl(TARGET, F_GETFD);
  EXPECT_NE(flags, -1);
  EXPECT_NE(flags & FD_CLOEXEC, 0);

  LIBC_NAMESPACE::close(TARGET);
  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// dup3 on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsDup3FchdirTest, Dup3BadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::dup3(-1, 62, 0), Fails(EBADF));
}

// ── fchdir ───────────────────────────────────────────────────────────────────

// fchdir changes cwd; getcwd confirms; chdir restores.
TEST_F(LlvmLibcWindowsDup3FchdirTest, FchdirChangesDirectory) {
  constexpr const char *SUBDIR = "fchdir_testdir";
  char orig[4096] = {};
  ASSERT_NE(LIBC_NAMESPACE::getcwd(orig, sizeof(orig)),
            static_cast<char *>(nullptr));

  ASSERT_THAT(LIBC_NAMESPACE::mkdir(SUBDIR, S_IRWXU), Succeeds(0));

  int dirfd = LIBC_NAMESPACE::open(SUBDIR, O_RDONLY);
  ASSERT_GT(dirfd, 0);

  ASSERT_THAT(LIBC_NAMESPACE::fchdir(dirfd), Succeeds(0));
  LIBC_NAMESPACE::close(dirfd);

  char after[4096] = {};
  ASSERT_NE(LIBC_NAMESPACE::getcwd(after, sizeof(after)),
            static_cast<char *>(nullptr));
  EXPECT_NE(__builtin_strcmp(orig, after), 0);

  ASSERT_THAT(LIBC_NAMESPACE::chdir(orig), Succeeds(0));
  ASSERT_THAT(LIBC_NAMESPACE::rmdir(SUBDIR), Succeeds(0));
}

// fchdir on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsDup3FchdirTest, FchdirBadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::fchdir(-1), Fails(EBADF));
}
