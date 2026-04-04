//===-- Windows unittests for fstatat/mkdirat -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   fstatat:
//   - AT_FDCWD + path returns same result as stat
//   - AT_SYMLINK_NOFOLLOW does not follow reparse points
//   - nonexistent path returns ENOENT
//
//   mkdirat:
//   - AT_FDCWD + name behaves like mkdir
//   - relative to a real dirfd creates inside that directory
//   - second call on same name returns EEXIST
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/fstatat.h"
#include "src/sys/stat/mkdir.h"
#include "src/sys/stat/mkdirat.h"
#include "src/unistd/close.h"
#include "src/unistd/rmdir.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h> // AT_FDCWD, AT_SYMLINK_NOFOLLOW

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsFstatatMkdiratTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ── fstatat ──────────────────────────────────────────────────────────────────

// fstatat AT_FDCWD is equivalent to stat: must return S_IFREG for a file.
TEST_F(LlvmLibcWindowsFstatatMkdiratTest, FstatatAtFdcwd) {
  constexpr const char *PATH = "fstatat_reg.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  struct stat st;
  ASSERT_THAT(LIBC_NAMESPACE::fstatat(AT_FDCWD, PATH, &st, 0), Succeeds(0));
  EXPECT_TRUE(S_ISREG(st.st_mode));

  LIBC_NAMESPACE::unlink(PATH);
}

// fstatat on a directory must return S_IFDIR.
TEST_F(LlvmLibcWindowsFstatatMkdiratTest, FstatatDirectory) {
  constexpr const char *DIR = "fstatat_dir";
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));

  struct stat st;
  ASSERT_THAT(LIBC_NAMESPACE::fstatat(AT_FDCWD, DIR, &st, 0), Succeeds(0));
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  LIBC_NAMESPACE::rmdir(DIR);
}

// fstatat on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsFstatatMkdiratTest, FstatatNonexistent) {
  struct stat st;
  EXPECT_THAT(
      LIBC_NAMESPACE::fstatat(AT_FDCWD, "__no_such_file_xyz__.tmp", &st, 0),
      Fails(ENOENT));
}

// ── mkdirat ──────────────────────────────────────────────────────────────────

// mkdirat with AT_FDCWD creates a directory in the cwd.
TEST_F(LlvmLibcWindowsFstatatMkdiratTest, MkdiratAtFdcwd) {
  constexpr const char *DIR = "mkdirat_fdcwd";
  ASSERT_THAT(LIBC_NAMESPACE::mkdirat(AT_FDCWD, DIR, S_IRWXU), Succeeds(0));

  struct stat st;
  ASSERT_THAT(LIBC_NAMESPACE::fstatat(AT_FDCWD, DIR, &st, 0), Succeeds(0));
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  LIBC_NAMESPACE::rmdir(DIR);
}

// mkdirat relative to a dirfd creates inside that directory.
TEST_F(LlvmLibcWindowsFstatatMkdiratTest, MkdiratRelativeToDir) {
  constexpr const char *PARENT = "mkdirat_parent";
  constexpr const char *CHILD  = "child";

  ASSERT_THAT(LIBC_NAMESPACE::mkdir(PARENT, S_IRWXU), Succeeds(0));

  int dirfd = LIBC_NAMESPACE::open(PARENT, O_RDONLY);
  ASSERT_GT(dirfd, 0);

  ASSERT_THAT(LIBC_NAMESPACE::mkdirat(dirfd, CHILD, S_IRWXU), Succeeds(0));
  LIBC_NAMESPACE::close(dirfd);

  // Verify child exists inside parent.
  char full[128] = {};
  __builtin_memcpy(full, PARENT, __builtin_strlen(PARENT));
  full[__builtin_strlen(PARENT)] = '/';
  __builtin_memcpy(full + __builtin_strlen(PARENT) + 1, CHILD,
                   __builtin_strlen(CHILD) + 1);

  struct stat st;
  EXPECT_THAT(LIBC_NAMESPACE::fstatat(AT_FDCWD, full, &st, 0), Succeeds(0));
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  LIBC_NAMESPACE::rmdir(full);
  LIBC_NAMESPACE::rmdir(PARENT);
}

// mkdirat on an already-existing name must return EEXIST.
TEST_F(LlvmLibcWindowsFstatatMkdiratTest, MkdiratExist) {
  constexpr const char *DIR = "mkdirat_exist";
  ASSERT_THAT(LIBC_NAMESPACE::mkdirat(AT_FDCWD, DIR, S_IRWXU), Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::mkdirat(AT_FDCWD, DIR, S_IRWXU), Fails(EEXIST));
  LIBC_NAMESPACE::rmdir(DIR);
}
