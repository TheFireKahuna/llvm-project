//===-- Windows unittests for pread/pwrite --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pwrite writes at the given offset without advancing the file position
//   - pread reads from the given offset without advancing the file position
//   - pread/pwrite on a bad fd return EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/lseek.h"
#include "src/unistd/pread.h"
#include "src/unistd/pwrite.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include "hdr/types/off_t.h"
#include "hdr/stdio_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsPreadPwriteTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// pwrite at offset 4 overwrites 3 bytes; position must stay at 10 throughout.
// pread at offset 4 reads those 3 bytes back without advancing position.
TEST_F(LlvmLibcWindowsPreadPwriteTest, WriteReadAtOffset) {
  constexpr const char *PATH = "preadpwrite_test.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  constexpr char BASE[] = "0123456789";
  constexpr ssize_t LEN = sizeof(BASE) - 1;
  ASSERT_THAT(LIBC_NAMESPACE::write(fd, BASE, LEN), Succeeds(LEN));
  // Position is now 10.

  // pwrite must not move position.
  ASSERT_THAT(LIBC_NAMESPACE::pwrite(fd, "XYZ", 3, 4), Succeeds(ssize_t(3)));
  EXPECT_EQ(LIBC_NAMESPACE::lseek(fd, 0, SEEK_CUR), off_t(10));

  // pread must not move position.
  char buf[4] = {};
  ASSERT_THAT(LIBC_NAMESPACE::pread(fd, buf, 3, 4), Succeeds(ssize_t(3)));
  EXPECT_EQ(LIBC_NAMESPACE::lseek(fd, 0, SEEK_CUR), off_t(10));
  EXPECT_EQ(buf[0], 'X');
  EXPECT_EQ(buf[1], 'Y');
  EXPECT_EQ(buf[2], 'Z');

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}

// pread/pwrite on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsPreadPwriteTest, BadFd) {
  char buf[4] = {};
  EXPECT_THAT(LIBC_NAMESPACE::pread(-1, buf, 4, 0), Fails<ssize_t>(EBADF));
  EXPECT_THAT(LIBC_NAMESPACE::pwrite(-1, buf, 4, 0), Fails<ssize_t>(EBADF));
}
