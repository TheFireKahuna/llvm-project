//===-- Windows unittests for pipe ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - pipe creates a read end and a write end
//   - Data written to the write end is readable from the read end
//   - After closing the write end, read returns 0 (EOF)
//   - pipe2 with O_CLOEXEC sets FD_CLOEXEC on both fds
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/fcntl.h"
#include "src/unistd/close.h"
#include "src/unistd/pipe.h"
#include "src/unistd/pipe2.h"
#include "src/unistd/read.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsPipeTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// Write to the write end; read from the read end — data must match.
TEST_F(LlvmLibcWindowsPipeTest, WriteAndRead) {
  int fds[2] = {-1, -1};
  ASSERT_THAT(LIBC_NAMESPACE::pipe(fds), Succeeds(0));
  ASSERT_GT(fds[0], 0); // read end
  ASSERT_GT(fds[1], 0); // write end

  constexpr char DATA[] = "pipe test";
  constexpr ssize_t LEN = sizeof(DATA); // include NUL

  ASSERT_THAT(LIBC_NAMESPACE::write(fds[1], DATA, LEN), Succeeds(LEN));

  char buf[sizeof(DATA)] = {};
  ASSERT_THAT(LIBC_NAMESPACE::read(fds[0], buf, LEN), Succeeds(LEN));
  EXPECT_EQ(__builtin_memcmp(buf, DATA, LEN), 0);

  LIBC_NAMESPACE::close(fds[0]);
  LIBC_NAMESPACE::close(fds[1]);
}

// After closing the write end, a read must return 0 (EOF).
TEST_F(LlvmLibcWindowsPipeTest, EofAfterWriteClose) {
  int fds[2] = {-1, -1};
  ASSERT_THAT(LIBC_NAMESPACE::pipe(fds), Succeeds(0));

  LIBC_NAMESPACE::close(fds[1]); // close write end

  char buf[4];
  EXPECT_THAT(LIBC_NAMESPACE::read(fds[0], buf, sizeof(buf)),
              Succeeds(ssize_t(0)));

  LIBC_NAMESPACE::close(fds[0]);
}

// pipe2 with O_CLOEXEC: both fds must have FD_CLOEXEC set.
TEST_F(LlvmLibcWindowsPipeTest, Pipe2Cloexec) {
  int fds[2] = {-1, -1};
  ASSERT_THAT(LIBC_NAMESPACE::pipe2(fds, O_CLOEXEC), Succeeds(0));

  EXPECT_EQ(LIBC_NAMESPACE::fcntl(fds[0], F_GETFD), FD_CLOEXEC);
  EXPECT_EQ(LIBC_NAMESPACE::fcntl(fds[1], F_GETFD), FD_CLOEXEC);

  LIBC_NAMESPACE::close(fds[0]);
  LIBC_NAMESPACE::close(fds[1]);
}
