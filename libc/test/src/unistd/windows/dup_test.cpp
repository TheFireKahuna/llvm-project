//===-- Windows unittests for dup/dup2/dup3 -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - dup returns a new fd referencing the same open file description
//   - dup2 duplicates to a specific fd number
//   - Writing via one fd and reading via the dup'd fd returns the same bytes
//   - dup on a bad fd returns EBADF
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/unistd/close.h"
#include "src/unistd/dup.h"
#include "src/unistd/dup2.h"
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
using LlvmLibcWindowsDupTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// dup: write via original fd, read via dup'd fd returns the same bytes.
TEST_F(LlvmLibcWindowsDupTest, DupSharesDescription) {
  constexpr const char *PATH = "dup_share.tmp";
  constexpr char DATA[] = "dup test";
  constexpr ssize_t LEN = sizeof(DATA) - 1;

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  int fd2 = LIBC_NAMESPACE::dup(fd);
  ASSERT_GT(fd2, 0);
  ASSERT_NE(fd, fd2);

  ASSERT_THAT(LIBC_NAMESPACE::write(fd, DATA, LEN), Succeeds(LEN));

  // Seek to start via dup'd fd — both fds share the same description so the
  // seek is visible to the original fd too.
  ASSERT_THAT(LIBC_NAMESPACE::lseek(fd2, 0, SEEK_SET), Succeeds(off_t(0)));

  char buf[sizeof(DATA)] = {};
  ASSERT_THAT(LIBC_NAMESPACE::read(fd2, buf, LEN), Succeeds(LEN));
  EXPECT_EQ(__builtin_memcmp(buf, DATA, LEN), 0);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::close(fd2);
  LIBC_NAMESPACE::remove(PATH);
}

// dup2: duplicates to the exact fd number requested.
TEST_F(LlvmLibcWindowsDupTest, Dup2SpecificFd) {
  constexpr const char *PATH = "dup2_specific.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  // Choose a target fd number that's unlikely to be in use.
  constexpr int TARGET = 50;
  int fd2 = LIBC_NAMESPACE::dup2(fd, TARGET);
  ASSERT_EQ(fd2, TARGET);

  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::close(fd2);
  LIBC_NAMESPACE::remove(PATH);
}

// dup on a bad fd must return EBADF.
TEST_F(LlvmLibcWindowsDupTest, BadFd) {
  EXPECT_THAT(LIBC_NAMESPACE::dup(-1), Fails(EBADF));
}
