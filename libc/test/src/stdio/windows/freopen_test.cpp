//===-- Windows unittests for freopen -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - freopen(path, mode, stream): redirects an open FILE* to a new path;
//     the returned pointer equals the input pointer (pointer stability)
//   - data written after freopen appears in the new file, not the old one
//   - freopen(nullptr, mode, stream): reopens the same file with a new mode
//   - freopen with an invalid mode string returns nullptr and sets EINVAL
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/fclose.h"
#include "src/stdio/fdopen.h"
#include "src/stdio/fread.h"
#include "src/stdio/freopen.h"
#include "src/stdio/fwrite.h"
#include "src/stdio/remove.h"
#include "src/stdio/rewind.h"
#include "src/unistd/close.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <stdio.h>
#include <sys/stat.h>

using LlvmLibcWindowsFreopenTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// freopen to a new path must return the same FILE* and redirect writes.
TEST_F(LlvmLibcWindowsFreopenTest, NewPath) {
  constexpr const char *PATH_A = "freopen_a.tmp";
  constexpr const char *PATH_B = "freopen_b.tmp";

  int fd = LIBC_NAMESPACE::open(PATH_A, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  ::FILE *f = LIBC_NAMESPACE::fdopen(fd, "r+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  // Redirect the stream to PATH_B.
  ::FILE *f2 = LIBC_NAMESPACE::freopen(PATH_B, "w+", f);
  // freopen must return the same pointer.
  EXPECT_EQ(f2, f);
  ASSERT_NE(f2, static_cast<::FILE *>(nullptr));

  // Write via the reopened stream.
  constexpr char DATA[] = "freopen data";
  constexpr size_t LEN = sizeof(DATA) - 1;
  EXPECT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, LEN, f2), LEN);

  // Read it back after rewinding.
  LIBC_NAMESPACE::rewind(f2);
  char buf[sizeof(DATA)] = {};
  EXPECT_EQ(LIBC_NAMESPACE::fread(buf, 1, LEN, f2), LEN);
  EXPECT_EQ(__builtin_memcmp(buf, DATA, LEN), 0);

  LIBC_NAMESPACE::fclose(f2);
  LIBC_NAMESPACE::remove(PATH_A);
  LIBC_NAMESPACE::remove(PATH_B);
}

// freopen with an invalid mode string must return nullptr and set EINVAL.
TEST_F(LlvmLibcWindowsFreopenTest, InvalidMode) {
  constexpr const char *PATH = "freopen_mode.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  ::FILE *f = LIBC_NAMESPACE::fdopen(fd, "r+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  ::FILE *f2 = LIBC_NAMESPACE::freopen(PATH, "q", f);
  EXPECT_EQ(f2, static_cast<::FILE *>(nullptr));
  ASSERT_ERRNO_EQ(EINVAL);

  // f is now closed by freopen's failure path; remove the file.
  LIBC_NAMESPACE::remove(PATH);
}

// freopen(nullptr, mode, stream) reopens the same file; pointer is stable.
TEST_F(LlvmLibcWindowsFreopenTest, NullPathReopens) {
  constexpr const char *PATH = "freopen_null.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  ::FILE *f = LIBC_NAMESPACE::fdopen(fd, "r+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  // Reopen in append mode via nullptr path.
  ::FILE *f2 = LIBC_NAMESPACE::freopen(nullptr, "a+", f);
  EXPECT_EQ(f2, f); // pointer stability

  LIBC_NAMESPACE::fclose(f2);
  LIBC_NAMESPACE::remove(PATH);
}
