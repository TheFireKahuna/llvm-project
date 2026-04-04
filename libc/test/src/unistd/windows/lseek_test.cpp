//===-- Windows unittests for lseek ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - SEEK_SET positions the file offset from the beginning
//   - SEEK_CUR advances the offset relative to current position
//   - SEEK_END positions from end of file
//   - lseek on a bad fd returns EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/lseek.h"
#include "src/unistd/read.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include "hdr/stdio_macros.h"
#include "hdr/types/off_t.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsLseekTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcWindowsLseekTest, SeekSetCurEnd) {
  constexpr const char *PATH = "lseek.tmp";
  // Write known data.
  constexpr char DATA[] = "0123456789"; // 10 bytes
  constexpr ssize_t LEN = sizeof(DATA) - 1;

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::write(fd, DATA, LEN), Succeeds(LEN));

  // SEEK_SET to byte 3 — read one byte, expect '3'.
  ASSERT_THAT(LIBC_NAMESPACE::lseek(fd, 3, SEEK_SET), Succeeds(off_t(3)));
  char c = 0;
  ASSERT_THAT(LIBC_NAMESPACE::read(fd, &c, 1), Succeeds(ssize_t(1)));
  EXPECT_EQ(c, '3');

  // SEEK_CUR back by 1 — position is now 3 again.
  ASSERT_THAT(LIBC_NAMESPACE::lseek(fd, -1, SEEK_CUR), Succeeds(off_t(3)));

  // SEEK_END -2 — last two bytes are '8','9'; read one, expect '8'.
  ASSERT_THAT(LIBC_NAMESPACE::lseek(fd, -2, SEEK_END), Succeeds(off_t(LEN - 2)));
  ASSERT_THAT(LIBC_NAMESPACE::read(fd, &c, 1), Succeeds(ssize_t(1)));
  EXPECT_EQ(c, '8');

  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));
  LIBC_NAMESPACE::remove(PATH);
}

TEST_F(LlvmLibcWindowsLseekTest, BadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::lseek(-1, 0, SEEK_SET), Fails<off_t>(EBADF));
}
