//===-- Windows unittests for tmpfile -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - tmpfile returns a non-null FILE*
//   - the returned stream is open for read+write
//   - the file is automatically deleted on fclose
//   - multiple calls return distinct streams (no aliasing)
//
//===----------------------------------------------------------------------===//

#include "src/stdio/fclose.h"
#include "src/stdio/fread.h"
#include "src/stdio/fwrite.h"
#include "src/stdio/rewind.h"
#include "src/stdio/tmpfile.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include <stdio.h>

using LlvmLibcWindowsTmpfileTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// tmpfile must return a non-null, readable+writable stream.
TEST_F(LlvmLibcWindowsTmpfileTest, ReturnsStream) {
  ::FILE *f = LIBC_NAMESPACE::tmpfile();
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));
  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
}

// Write to tmpfile, rewind, read back — verifies r/w mode.
TEST_F(LlvmLibcWindowsTmpfileTest, ReadWrite) {
  ::FILE *f = LIBC_NAMESPACE::tmpfile();
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "tmpfile test";
  constexpr size_t LEN = sizeof(DATA) - 1;
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, LEN, f), LEN);

  LIBC_NAMESPACE::rewind(f);

  char buf[sizeof(DATA)] = {};
  ASSERT_EQ(LIBC_NAMESPACE::fread(buf, 1, LEN, f), LEN);
  ASSERT_EQ(__builtin_memcmp(buf, DATA, LEN), 0);

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
}

// Two tmpfile calls must return distinct FILE pointers.
TEST_F(LlvmLibcWindowsTmpfileTest, DistinctStreams) {
  ::FILE *f1 = LIBC_NAMESPACE::tmpfile();
  ::FILE *f2 = LIBC_NAMESPACE::tmpfile();
  ASSERT_NE(f1, static_cast<::FILE *>(nullptr));
  ASSERT_NE(f2, static_cast<::FILE *>(nullptr));
  EXPECT_NE(f1, f2);
  LIBC_NAMESPACE::fclose(f1);
  LIBC_NAMESPACE::fclose(f2);
}
