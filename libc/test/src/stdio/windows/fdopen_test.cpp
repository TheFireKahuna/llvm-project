//===-- Windows unittests for fdopen --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - fdopen wraps a disk fd in a FILE* open for read+write
//   - data written via fwrite is readable via fread after rewind
//   - a second fdopen call on the same fd returns the cached FILE*
//   - fdopen on a bad fd returns nullptr and sets EBADF
//   - fdopen with an invalid mode string returns nullptr and sets EINVAL
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/fclose.h"
#include "src/stdio/fdopen.h"
#include "src/stdio/fread.h"
#include "src/stdio/fwrite.h"
#include "src/stdio/remove.h"
#include "src/stdio/rewind.h"
#include "src/unistd/close.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <stdio.h>
#include <sys/stat.h>

using LlvmLibcWindowsFdopenTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// fdopen wraps an fd; fwrite then rewind then fread must roundtrip.
TEST_F(LlvmLibcWindowsFdopenTest, ReadWrite) {
  constexpr const char *PATH = "fdopen_test.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  ::FILE *f = LIBC_NAMESPACE::fdopen(fd, "r+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "fdopen roundtrip";
  constexpr size_t LEN = sizeof(DATA) - 1;
  EXPECT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, LEN, f), LEN);

  LIBC_NAMESPACE::rewind(f);

  char buf[sizeof(DATA)] = {};
  EXPECT_EQ(LIBC_NAMESPACE::fread(buf, 1, LEN, f), LEN);
  EXPECT_EQ(__builtin_memcmp(buf, DATA, LEN), 0);

  // fclose also closes the underlying fd.
  LIBC_NAMESPACE::fclose(f);
  LIBC_NAMESPACE::remove(PATH);
}

// A second fdopen call on the same fd must return the cached FILE*.
TEST_F(LlvmLibcWindowsFdopenTest, IdempotentSecondCall) {
  constexpr const char *PATH = "fdopen_idem.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  ::FILE *f1 = LIBC_NAMESPACE::fdopen(fd, "r+");
  ASSERT_NE(f1, static_cast<::FILE *>(nullptr));

  ::FILE *f2 = LIBC_NAMESPACE::fdopen(fd, "r+");
  EXPECT_EQ(f1, f2);

  LIBC_NAMESPACE::fclose(f1);
  LIBC_NAMESPACE::remove(PATH);
}

// fdopen on a bad fd must return nullptr and set EBADF.
TEST_F(LlvmLibcWindowsFdopenTest, BadFd) {
  ::FILE *f = LIBC_NAMESPACE::fdopen(-1, "r");
  EXPECT_EQ(f, static_cast<::FILE *>(nullptr));
  ASSERT_ERRNO_EQ(EBADF);
}

// fdopen with an invalid mode string must return nullptr and set EINVAL.
TEST_F(LlvmLibcWindowsFdopenTest, InvalidMode) {
  constexpr const char *PATH = "fdopen_mode.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);

  ::FILE *f = LIBC_NAMESPACE::fdopen(fd, "q");
  EXPECT_EQ(f, static_cast<::FILE *>(nullptr));
  ASSERT_ERRNO_EQ(EINVAL);

  // fdopen failed so fd is still open; close and clean up.
  LIBC_NAMESPACE::close(fd);
  LIBC_NAMESPACE::remove(PATH);
}
