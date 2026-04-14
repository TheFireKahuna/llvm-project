//===-- Windows unittests for symlinkat/readlinkat ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// NTFS symlinks require Developer Mode or SeCreateSymbolicLinkPrivilege.
// Tests that create symlinks are privilege-adaptive:
//   - if symlinkat succeeds: readlinkat returns the target string
//   - if symlinkat fails with EPERM: the test passes (no privilege is fine)
//
// POSIX compliance points tested:
//   - symlinkat(target, AT_FDCWD, link) — baseline AT_FDCWD path
//   - symlinkat(target, dirfd, name) — creates symlink relative to dirfd
//   - readlinkat(AT_FDCWD, link, buf, size) — reads symlink via AT_FDCWD
//   - readlinkat(dirfd, name, buf, size) — reads symlink relative to dirfd
//   - readlinkat on a regular file → EINVAL
//   - readlinkat on a nonexistent path → ENOENT
//   - readlinkat with a bad dirfd → EBADF
//   - symlinkat with a bad dirfd → EBADF
//
// symlinkat and readlinkat each call resolve_at_path() independently — their
// dirfd handling is not exercised by symlink() or readlink().
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/mkdir.h"
#include "src/unistd/close.h"
#include "src/unistd/readlinkat.h"
#include "src/unistd/rmdir.h"
#include "src/unistd/symlinkat.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h> // AT_FDCWD

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsSymlinkatReadlinkatTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// symlinkat + readlinkat via AT_FDCWD — baseline case.
TEST_F(LlvmLibcWindowsSymlinkatReadlinkatTest, AtFdcwd) {
  constexpr const char *TARGET = "symlat_target.tmp";
  constexpr const char *LINK   = "symlat_link.lnk";

  int fd = LIBC_NAMESPACE::open(TARGET, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  int ret = LIBC_NAMESPACE::symlinkat(TARGET, AT_FDCWD, LINK);
  if (ret == -1) {
    EXPECT_TRUE(libc_errno == EPERM || libc_errno == ENOTSUP);
    LIBC_NAMESPACE::unlink(TARGET);
    return;
  }

  char buf[256] = {};
  ssize_t n = LIBC_NAMESPACE::readlinkat(AT_FDCWD, LINK, buf, sizeof(buf) - 1);
  EXPECT_GT(n, ssize_t(0));
  if (n > 0) {
    buf[n] = '\0';
    const char *needle = TARGET;
    size_t nlen = __builtin_strlen(needle);
    size_t blen = static_cast<size_t>(n);
    bool found = false;
    for (size_t i = 0; i + nlen <= blen; ++i) {
      if (__builtin_memcmp(buf + i, needle, nlen) == 0) { found = true; break; }
    }
    EXPECT_TRUE(found);
  }

  LIBC_NAMESPACE::unlink(LINK);
  LIBC_NAMESPACE::unlink(TARGET);
}

// symlinkat(target, dirfd, name): creates symlink inside the directory.
// readlinkat(dirfd, name, …): reads it back relative to the same dirfd.
// This exercises resolve_at_path(dfd, ...) with a real fd — not AT_FDCWD.
TEST_F(LlvmLibcWindowsSymlinkatReadlinkatTest, RelativeToDir) {
  constexpr const char *DIR    = "symlat_dir";
  constexpr const char *TARGET = "symlat_rel_target.tmp";
  constexpr const char *NAME   = "symlat_rel_link.lnk";

  // Create a source file to point at (target need not exist for symlinkat).
  int sfd = LIBC_NAMESPACE::open(TARGET, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(sfd, 0);
  LIBC_NAMESPACE::close(sfd);

  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));
  int dirfd = LIBC_NAMESPACE::open(DIR, O_RDONLY);
  ASSERT_GT(dirfd, 0);

  // Create symlink inside DIR, using absolute target so readlinkat can verify.
  int ret = LIBC_NAMESPACE::symlinkat(TARGET, dirfd, NAME);
  if (ret == -1) {
    EXPECT_TRUE(libc_errno == EPERM || libc_errno == ENOTSUP);
    LIBC_NAMESPACE::close(dirfd);
    LIBC_NAMESPACE::unlink(TARGET);
    LIBC_NAMESPACE::rmdir(DIR);
    return;
  }

  // readlinkat with the same dirfd should find the symlink.
  char buf[256] = {};
  ssize_t n = LIBC_NAMESPACE::readlinkat(dirfd, NAME, buf, sizeof(buf) - 1);
  EXPECT_GT(n, ssize_t(0));
  if (n > 0) {
    buf[n] = '\0';
    const char *needle = TARGET;
    size_t nlen = __builtin_strlen(needle);
    size_t blen = static_cast<size_t>(n);
    bool found = false;
    for (size_t i = 0; i + nlen <= blen; ++i) {
      if (__builtin_memcmp(buf + i, needle, nlen) == 0) { found = true; break; }
    }
    EXPECT_TRUE(found);
  }

  LIBC_NAMESPACE::close(dirfd);

  // Cleanup.
  char full[128] = {};
  size_t dlen = __builtin_strlen(DIR);
  size_t flen = __builtin_strlen(NAME);
  __builtin_memcpy(full, DIR, dlen);
  full[dlen] = '/';
  __builtin_memcpy(full + dlen + 1, NAME, flen + 1);
  LIBC_NAMESPACE::unlink(full);
  LIBC_NAMESPACE::unlink(TARGET);
  LIBC_NAMESPACE::rmdir(DIR);
}

// readlinkat on a regular file → EINVAL.
TEST_F(LlvmLibcWindowsSymlinkatReadlinkatTest, ReadlinkatRegularFile) {
  constexpr const char *PATH = "readlinkat_reg.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  char buf[64] = {};
  EXPECT_THAT(
      LIBC_NAMESPACE::readlinkat(AT_FDCWD, PATH, buf, sizeof(buf)),
      Fails<ssize_t>(EINVAL));
  LIBC_NAMESPACE::unlink(PATH);
}

// readlinkat on a nonexistent path → ENOENT.
TEST_F(LlvmLibcWindowsSymlinkatReadlinkatTest, ReadlinkatNonexistent) {
  char buf[64] = {};
  EXPECT_THAT(
      LIBC_NAMESPACE::readlinkat(AT_FDCWD, "__no_such_link_xyz__.lnk",
                                 buf, sizeof(buf)),
      Fails<ssize_t>(ENOENT));
}

// readlinkat with bad dirfd → EBADF.
TEST_F(LlvmLibcWindowsSymlinkatReadlinkatTest, ReadlinkatBadDirfd) {
  char buf[64] = {};
  EXPECT_THAT(
      LIBC_NAMESPACE::readlinkat(9999, "some_link.lnk", buf, sizeof(buf)),
      Fails<ssize_t>(EBADF));
}

// symlinkat with bad dirfd → EBADF.
TEST_F(LlvmLibcWindowsSymlinkatReadlinkatTest, SymlinkatBadDirfd) {
  EXPECT_THAT(
      LIBC_NAMESPACE::symlinkat("target.tmp", 9999, "link.lnk"),
      Fails(EBADF));
}
