//===-- Windows unittests for fstat ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - fstat on a valid fd succeeds and reports S_IFREG
//   - fstat on an invalid fd returns EBADF
//   - fstat reports correct st_size after a write
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
using LlvmLibcWindowsFStatTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// fstat on a valid fd must succeed and report S_IFREG.
TEST_F(LlvmLibcWindowsFStatTest, ValidFd) {
  constexpr const char *PATH = "fstat_valid.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);

  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd, &buf), Succeeds(0));
  EXPECT_TRUE((buf.st_mode & S_IFMT) == S_IFREG);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}

// fstat on an invalid fd must return EBADF.
TEST_F(LlvmLibcWindowsFStatTest, InvalidFd) {
  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::fstat(-1, &buf), Fails(EBADF));
}

// st_size must reflect bytes written before the fstat call.
TEST_F(LlvmLibcWindowsFStatTest, SizeAfterWrite) {
  constexpr const char *PATH = "fstat_size.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);

  constexpr char DATA[] = "hello";
  constexpr ssize_t DATA_LEN = sizeof(DATA) - 1; // exclude NUL
  ASSERT_EQ(LIBC_NAMESPACE::write(fd, DATA, DATA_LEN), DATA_LEN);

  // Flush is implicit on Windows — NtWriteFile is synchronous for regular
  // files, so st_size is visible immediately.
  struct stat buf;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(fd, &buf), Succeeds(0));
  EXPECT_EQ(static_cast<ssize_t>(buf.st_size), DATA_LEN);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::unlink(PATH);
}
