//===-- Windows smoke tests for fscanf ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests fscanf on NTPOSIX using fopen() with CWD-relative paths to avoid
// the testdata-directory infrastructure that fails on this target.
//
// Compliance points tested:
//   - fscanf reads formatted data from a FILE*
//   - %s conversion reads a whitespace-delimited string
//   - %d conversion reads a decimal integer
//   - %c conversion reads a single character (no whitespace skip)
//   - fscanf returns the number of items successfully matched
//
//===----------------------------------------------------------------------===//

#include "src/stdio/fclose.h"
#include "src/stdio/fopen.h"
#include "src/stdio/fscanf.h"
#include "src/stdio/fwrite.h"
#include "src/stdio/remove.h"
#include "src/stdio/rewind.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include <stdio.h>

using LlvmLibcWindowsFscanfTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// Write a string, rewind, fscanf it back with %s.
TEST_F(LlvmLibcWindowsFscanfTest, ReadString) {
  constexpr const char *PATH = "fscanf_str.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "hello";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, sizeof(DATA) - 1, f),
            sizeof(DATA) - 1);

  LIBC_NAMESPACE::rewind(f);

  char buf[32] = {};
  int ret = LIBC_NAMESPACE::fscanf(f, "%s", buf);
  ASSERT_EQ(ret, 1);
  ASSERT_STREQ(buf, "hello");

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// Read an integer with %d.
TEST_F(LlvmLibcWindowsFscanfTest, ReadInteger) {
  constexpr const char *PATH = "fscanf_int.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "42";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, sizeof(DATA) - 1, f),
            sizeof(DATA) - 1);

  LIBC_NAMESPACE::rewind(f);

  int val = 0;
  int ret = LIBC_NAMESPACE::fscanf(f, "%d", &val);
  ASSERT_EQ(ret, 1);
  ASSERT_EQ(val, 42);

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// Multiple conversions in one call.
TEST_F(LlvmLibcWindowsFscanfTest, MultipleConversions) {
  constexpr const char *PATH = "fscanf_multi.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "abc 123";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, sizeof(DATA) - 1, f),
            sizeof(DATA) - 1);

  LIBC_NAMESPACE::rewind(f);

  char word[32] = {};
  int num = 0;
  int ret = LIBC_NAMESPACE::fscanf(f, "%s %d", word, &num);
  ASSERT_EQ(ret, 2);
  ASSERT_STREQ(word, "abc");
  ASSERT_EQ(num, 123);

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// fscanf with %c does not skip leading whitespace.
TEST_F(LlvmLibcWindowsFscanfTest, CharConversion) {
  constexpr const char *PATH = "fscanf_char.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "XY";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, sizeof(DATA) - 1, f),
            sizeof(DATA) - 1);

  LIBC_NAMESPACE::rewind(f);

  char c1 = 0, c2 = 0;
  int ret = LIBC_NAMESPACE::fscanf(f, "%c%c", &c1, &c2);
  ASSERT_EQ(ret, 2);
  ASSERT_EQ(c1, 'X');
  ASSERT_EQ(c2, 'Y');

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// Write then read-back with separate fopen calls.
TEST_F(LlvmLibcWindowsFscanfTest, WriteToFileAndScanBack) {
  constexpr const char *PATH = "fscanf_rw.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "1234567890\n";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, sizeof(DATA) - 1, f),
            sizeof(DATA) - 1);
  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);

  f = LIBC_NAMESPACE::fopen(PATH, "r");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  char buf[32] = {};
  int ret = LIBC_NAMESPACE::fscanf(f, "%s", buf);
  ASSERT_EQ(ret, 1);
  ASSERT_STREQ(buf, "1234567890");

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}
