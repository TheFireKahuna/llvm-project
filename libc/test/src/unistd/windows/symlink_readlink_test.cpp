//===-- Windows unittests for symlink/readlink ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// NTFS symlinks require Developer Mode or SeCreateSymbolicLinkPrivilege.
// These tests handle both the privileged and unprivileged cases:
//
//   - if symlink succeeds: readlink returns the target string; unlink cleans up
//   - if symlink fails with EPERM: the test passes (no privilege is fine)
//   - readlink on a regular file → EINVAL
//   - readlink on a nonexistent path → ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/unistd/close.h"
#include "src/unistd/readlink.h"
#include "src/unistd/symlink.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsSymlinkReadlinkTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// If we have privilege, symlink must succeed and readlink must return the
// target. If we lack privilege, symlink returns EPERM — that is also correct
// POSIX behaviour for this platform.
TEST_F(LlvmLibcWindowsSymlinkReadlinkTest, SymlinkAndReadlink) {
  constexpr const char *TARGET = "symlink_target.tmp";
  constexpr const char *LINK   = "symlink_link.lnk";

  int tfd = LIBC_NAMESPACE::open(TARGET, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(tfd, 0);
  LIBC_NAMESPACE::close(tfd);

  int ret = LIBC_NAMESPACE::symlink(TARGET, LINK);
  if (ret == -1) {
    // No privilege — verify errno is EPERM (or ENOTSUP) and stop.
    EXPECT_TRUE(libc_errno == EPERM || libc_errno == ENOTSUP);
    LIBC_NAMESPACE::unlink(TARGET);
    return;
  }

  // Privileged path: readlink must return the target string.
  char buf[256] = {};
  ssize_t n = LIBC_NAMESPACE::readlink(LINK, buf, sizeof(buf) - 1);
  EXPECT_GT(n, ssize_t(0));
  if (n > 0) {
    buf[n] = '\0';
    // The stored target must contain "symlink_target.tmp".
    bool found = false;
    const char *needle = TARGET;
    size_t nlen = __builtin_strlen(needle);
    size_t blen = static_cast<size_t>(n);
    for (size_t i = 0; i + nlen <= blen; ++i) {
      if (__builtin_memcmp(buf + i, needle, nlen) == 0) { found = true; break; }
    }
    EXPECT_TRUE(found);
  }

  LIBC_NAMESPACE::unlink(LINK);
  LIBC_NAMESPACE::unlink(TARGET);
}

// readlink on a regular file must return EINVAL.
TEST_F(LlvmLibcWindowsSymlinkReadlinkTest, ReadlinkOnRegularFile) {
  constexpr const char *PATH = "readlink_reg.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  char buf[64] = {};
  EXPECT_THAT(LIBC_NAMESPACE::readlink(PATH, buf, sizeof(buf)),
              Fails<ssize_t>(EINVAL));
  LIBC_NAMESPACE::unlink(PATH);
}

// readlink on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsSymlinkReadlinkTest, ReadlinkNonexistent) {
  char buf[64] = {};
  EXPECT_THAT(
      LIBC_NAMESPACE::readlink("__no_such_link_xyz__.lnk", buf, sizeof(buf)),
      Fails<ssize_t>(ENOENT));
}
