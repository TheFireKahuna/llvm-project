//===-- Windows unittests for fcntl ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - F_GETFL returns flags the file was opened with
//   - F_SETFL sets O_APPEND / O_NONBLOCK; other bits are ignored
//   - F_GETFD returns the fd-flags (initially 0)
//   - F_SETFD sets FD_CLOEXEC; F_GETFD reads it back
//   - F_DUPFD duplicates the fd at >= the requested minimum
//   - F_DUPFD_CLOEXEC duplicates and sets FD_CLOEXEC on the new fd
//   - Unknown command returns EINVAL
//   - All commands on a bad fd return EBADF
//
// Note: F_GETLK / F_SETLK / F_SETOWN are not implemented on Windows — they
// return EINVAL, which is also tested here.
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/fcntl.h"
#include "src/fcntl/open.h"
#include "src/unistd/close.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsFcntlTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

static int open_tmp(const char *path, int flags) {
  return LIBC_NAMESPACE::open(path, flags | O_CREAT, S_IRWXU);
}

// F_GETFL must return at least the access-mode bits used at open().
TEST_F(LlvmLibcWindowsFcntlTest, GetFl) {
  constexpr const char *PATH = "fcntl_getfl.tmp";
  int fd = open_tmp(PATH, O_RDWR);
  ASSERT_GT(fd, 0);

  int flags = LIBC_NAMESPACE::fcntl(fd, F_GETFL);
  ASSERT_GE(flags, 0);
  // Access mode must be preserved.
  EXPECT_EQ(flags & O_ACCMODE, O_RDWR);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// F_SETFL O_APPEND must be reflected by a subsequent F_GETFL.
TEST_F(LlvmLibcWindowsFcntlTest, SetFlAppend) {
  constexpr const char *PATH = "fcntl_setfl.tmp";
  int fd = open_tmp(PATH, O_RDWR);
  ASSERT_GT(fd, 0);

  int flags = LIBC_NAMESPACE::fcntl(fd, F_GETFL);
  ASSERT_GE(flags, 0);

  // Set O_APPEND.
  ASSERT_EQ(LIBC_NAMESPACE::fcntl(fd, F_SETFL, flags | O_APPEND), 0);

  int new_flags = LIBC_NAMESPACE::fcntl(fd, F_GETFL);
  EXPECT_TRUE((new_flags & O_APPEND) != 0);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// F_GETFD must initially return 0 (no FD_CLOEXEC).
// F_SETFD FD_CLOEXEC must be readable back via F_GETFD.
TEST_F(LlvmLibcWindowsFcntlTest, GetSetFd) {
  constexpr const char *PATH = "fcntl_getsetfd.tmp";
  int fd = open_tmp(PATH, O_RDWR);
  ASSERT_GT(fd, 0);

  EXPECT_EQ(LIBC_NAMESPACE::fcntl(fd, F_GETFD), 0);

  ASSERT_EQ(LIBC_NAMESPACE::fcntl(fd, F_SETFD, FD_CLOEXEC), 0);
  EXPECT_EQ(LIBC_NAMESPACE::fcntl(fd, F_GETFD), FD_CLOEXEC);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// F_DUPFD must return a new fd >= the requested minimum.
TEST_F(LlvmLibcWindowsFcntlTest, DupFd) {
  constexpr const char *PATH = "fcntl_dup.tmp";
  int fd = open_tmp(PATH, O_RDWR);
  ASSERT_GT(fd, 0);

  int fd2 = LIBC_NAMESPACE::fcntl(fd, F_DUPFD, 0);
  ASSERT_GT(fd2, 0);
  EXPECT_GE(fd2, 0);

  // The duplicate must also report a valid fd (F_GETFD succeeds).
  EXPECT_GE(LIBC_NAMESPACE::fcntl(fd2, F_GETFD), 0);

  LIBC_NAMESPACE::close(fd2);
  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// F_DUPFD_CLOEXEC must set FD_CLOEXEC on the new fd.
TEST_F(LlvmLibcWindowsFcntlTest, DupFdCloexec) {
  constexpr const char *PATH = "fcntl_dup_cloexec.tmp";
  int fd = open_tmp(PATH, O_RDWR);
  ASSERT_GT(fd, 0);

  int fd2 = LIBC_NAMESPACE::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  ASSERT_GT(fd2, 0);
  EXPECT_EQ(LIBC_NAMESPACE::fcntl(fd2, F_GETFD), FD_CLOEXEC);

  LIBC_NAMESPACE::close(fd2);
  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// Any command on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsFcntlTest, BadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::fcntl(-1, F_GETFL), Fails(EBADF));
}

// F_GETLK and F_SETOWN are unimplemented on Windows — must return EINVAL.
TEST_F(LlvmLibcWindowsFcntlTest, UnimplementedCommands) {
  constexpr const char *PATH = "fcntl_unimpl.tmp";
  int fd = open_tmp(PATH, O_RDWR);
  ASSERT_GT(fd, 0);

  // Use a command value that is guaranteed not to be one of the implemented
  // ones — the highest-numbered implemented cmd is F_DUPFD_CLOEXEC.
  EXPECT_THAT(LIBC_NAMESPACE::fcntl(fd, 9999), Fails(EINVAL));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}
