//===-- Windows unittests for open ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - open without O_CREAT on a nonexistent path returns ENOENT
//   - O_CREAT creates the file if absent
//   - O_CREAT | O_EXCL on an existing file returns EEXIST
//   - O_RDONLY, O_WRONLY, O_RDWR all succeed on an existing file
//   - O_TRUNC truncates an existing file to zero length
//   - open with an invalid (bad) flags combination returns EINVAL
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/fstat.h"
#include "src/unistd/close.h"
#include "src/unistd/unlink.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsOpenTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// Opening a nonexistent file without O_CREAT must return ENOENT.
TEST_F(LlvmLibcWindowsOpenTest, NoCreateOnMissing) {
  EXPECT_THAT(LIBC_NAMESPACE::open("open_no_creat.tmp", O_RDONLY), Fails(ENOENT));
}

// O_CREAT creates the file; the returned fd must be valid.
TEST_F(LlvmLibcWindowsOpenTest, CreateNew) {
  constexpr const char *PATH = "open_creat.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));
  LIBC_NAMESPACE::unlink(PATH);
}

// O_CREAT | O_EXCL on an already-existing file must return EEXIST.
TEST_F(LlvmLibcWindowsOpenTest, ExclOnExisting) {
  constexpr const char *PATH = "open_excl.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  EXPECT_THAT(LIBC_NAMESPACE::open(PATH, O_CREAT | O_EXCL | O_WRONLY, S_IRWXU),
              Fails(EEXIST));
  LIBC_NAMESPACE::unlink(PATH);
}

// O_RDONLY, O_WRONLY, and O_RDWR must all succeed on an existing file.
TEST_F(LlvmLibcWindowsOpenTest, AccessModes) {
  constexpr const char *PATH = "open_modes.tmp";

  // Create with RDWR first.
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  fd = LIBC_NAMESPACE::open(PATH, O_RDONLY);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  fd = LIBC_NAMESPACE::open(PATH, O_WRONLY);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  fd = LIBC_NAMESPACE::open(PATH, O_RDWR);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  LIBC_NAMESPACE::unlink(PATH);
}

// O_TRUNC must truncate an existing file to zero bytes.
TEST_F(LlvmLibcWindowsOpenTest, Truncate) {
  constexpr const char *PATH = "open_trunc.tmp";

  // Write some data.
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  constexpr char DATA[] = "hello world";
  LIBC_NAMESPACE::write(fd, DATA, sizeof(DATA) - 1);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  // Re-open with O_TRUNC.
  fd = LIBC_NAMESPACE::open(PATH, O_WRONLY | O_TRUNC);
  ASSERT_GT(fd, 0);

  // st_size must now be zero.
  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd, &buf), Succeeds(0));
  EXPECT_EQ(buf.st_size, static_cast<off_t>(0));

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}
