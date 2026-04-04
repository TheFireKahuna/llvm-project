//===-- Windows unittests for openat --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - openat with AT_FDCWD behaves identically to open
//   - openat with a directory fd opens a path relative to that directory
//   - openat O_CREAT|O_EXCL on an existing path returns EEXIST
//   - openat on a nonexistent path without O_CREAT returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/fcntl/openat.h"
#include "src/sys/stat/mkdir.h"
#include "src/unistd/close.h"
#include "src/unistd/rmdir.h"
#include "src/unistd/unlink.h"
#include "src/unistd/write.h"
#include "src/unistd/read.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h> // AT_FDCWD

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsOpenatTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// openat with AT_FDCWD creates a file in the cwd — same as open.
TEST_F(LlvmLibcWindowsOpenatTest, AtFdcwd) {
  constexpr const char *PATH = "openat_fdcwd.tmp";
  int fd = LIBC_NAMESPACE::openat(AT_FDCWD, PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  ASSERT_THAT(LIBC_NAMESPACE::write(fd, "hello", 5), Succeeds(ssize_t(5)));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// openat relative to a directory fd opens the file inside that directory.
TEST_F(LlvmLibcWindowsOpenatTest, RelativeToDir) {
  constexpr const char *DIR  = "openat_dir";
  constexpr const char *FILE = "openat_rel.tmp";

  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));

  // Open the directory to get a dirfd.
  int dirfd = LIBC_NAMESPACE::open(DIR, O_RDONLY);
  ASSERT_GT(dirfd, 0);

  int fd = LIBC_NAMESPACE::openat(dirfd, FILE, O_CREAT | O_RDWR, S_IRWXU);
  EXPECT_GT(fd, 0);
  if (fd > 0) LIBC_NAMESPACE::close(fd);

  LIBC_NAMESPACE::close(dirfd);

  // Clean up: build the path manually to unlink the file.
  // open the file via AT_FDCWD with its full relative path.
  char full[128] = {};
  __builtin_memcpy(full, DIR, __builtin_strlen(DIR));
  full[__builtin_strlen(DIR)] = '/';
  __builtin_memcpy(full + __builtin_strlen(DIR) + 1, FILE,
                   __builtin_strlen(FILE) + 1);
  LIBC_NAMESPACE::unlink(full);
  LIBC_NAMESPACE::rmdir(DIR);
}

// openat O_CREAT|O_EXCL on an existing path must return EEXIST.
TEST_F(LlvmLibcWindowsOpenatTest, ExclOnExisting) {
  constexpr const char *PATH = "openat_excl.tmp";
  int fd = LIBC_NAMESPACE::openat(AT_FDCWD, PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(
      LIBC_NAMESPACE::openat(AT_FDCWD, PATH, O_CREAT | O_EXCL | O_RDWR,
                             S_IRWXU),
      Fails(EEXIST));
  LIBC_NAMESPACE::unlink(PATH);
}

// openat without O_CREAT on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsOpenatTest, NonexistentNoCreate) {
  EXPECT_THAT(
      LIBC_NAMESPACE::openat(AT_FDCWD, "__no_file_xyz__.tmp", O_RDONLY),
      Fails(ENOENT));
}
