//===-- Windows smoke tests for ungetc ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests ungetc on NTPOSIX using fopen() with CWD-relative paths to avoid
// the testdata-directory infrastructure that fails on this target.
//
// Compliance points tested:
//   - ungetc pushes a character back onto the stream
//   - the pushed character is returned by the next fread
//   - ungetc(EOF, ...) returns EOF without modifying the stream
//   - ungetc after fseek succeeds (clears prior unget state)
//
//===----------------------------------------------------------------------===//

#include "hdr/stdio_macros.h"
#include "src/stdio/fclose.h"
#include "src/stdio/fopen.h"
#include "src/stdio/fread.h"
#include "src/stdio/fscanf.h"
#include "src/stdio/fseek.h"
#include "src/stdio/fwrite.h"
#include "src/stdio/remove.h"
#include "src/stdio/rewind.h"
#include "src/stdio/ungetc.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include <stdio.h>

using LlvmLibcWindowsUngetcTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// Push a char back with ungetc, then fread it.
TEST_F(LlvmLibcWindowsUngetcTest, UngetAndReadBack) {
  constexpr const char *PATH = "ungetc_readback.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "abcdef";
  constexpr size_t LEN = sizeof(DATA) - 1;
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, LEN, f), LEN);

  LIBC_NAMESPACE::rewind(f);

  // Read the first char, then push it back.
  char c;
  ASSERT_EQ(LIBC_NAMESPACE::fread(&c, 1, 1, f), size_t(1));
  ASSERT_EQ(c, 'a');

  ASSERT_EQ(LIBC_NAMESPACE::ungetc(int(c), f), int(c));

  // Now read the full string; should start with the ungotten 'a'.
  char buf[sizeof(DATA)] = {};
  ASSERT_EQ(LIBC_NAMESPACE::fread(buf, 1, LEN, f), LEN);
  ASSERT_STREQ(buf, DATA);

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// ungetc(EOF) must return EOF without doing anything.
TEST_F(LlvmLibcWindowsUngetcTest, UngetEOF) {
  constexpr const char *PATH = "ungetc_eof.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  ASSERT_EQ(LIBC_NAMESPACE::ungetc(EOF, f), EOF);

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// ungetc should succeed after fseek (clears the unget buffer).
TEST_F(LlvmLibcWindowsUngetcTest, UngetAfterSeek) {
  constexpr const char *PATH = "ungetc_seek.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "abcdef";
  constexpr size_t CONTENT_SIZE = sizeof(DATA); // includes NUL
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, CONTENT_SIZE, f), CONTENT_SIZE);
  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);

  f = LIBC_NAMESPACE::fopen(PATH, "r+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  // Read one char, then seek to start, then ungetc should succeed.
  char c;
  ASSERT_EQ(LIBC_NAMESPACE::fread(&c, 1, 1, f), size_t(1));
  ASSERT_EQ(c, 'a');

  ASSERT_EQ(LIBC_NAMESPACE::fseek(f, 0, SEEK_SET), 0);

  int unget_char = 'z';
  ASSERT_EQ(LIBC_NAMESPACE::ungetc(unget_char, f), unget_char);

  // Read back: 'z' followed by the original content.
  char new_data[CONTENT_SIZE + 1] = {};
  ASSERT_EQ(LIBC_NAMESPACE::fread(new_data, 1, CONTENT_SIZE + 1, f),
            CONTENT_SIZE + 1);
  ASSERT_STREQ(new_data, "zabcdef");

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// ungetc must succeed after fscanf consumes data up to EOF (no trailing
// whitespace).  Previously, fscanf's internal getc returned '\0' on EOF and
// the converter pushed that '\0' into the file buffer, leaving it in a state
// (read_limit != 0, pos == 0) that made the next ungetc fail.
TEST_F(LlvmLibcWindowsUngetcTest, UngetAfterFscanfEOF) {
  constexpr const char *PATH = "ungetc_fscanf_eof.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  // Write a number with NO trailing whitespace/newline so fscanf reads to EOF.
  constexpr char DATA[] = "42";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, sizeof(DATA) - 1, f),
            sizeof(DATA) - 1);

  LIBC_NAMESPACE::rewind(f);

  int val = 0;
  int ret = LIBC_NAMESPACE::fscanf(f, "%d", &val);
  ASSERT_EQ(ret, 1);
  ASSERT_EQ(val, 42);

  // This is the call that used to return EOF (-1) due to the corrupted buffer.
  ASSERT_EQ(LIBC_NAMESPACE::ungetc('X', f), int('X'));

  // The pushed-back character should be readable.
  char c = 0;
  ASSERT_EQ(LIBC_NAMESPACE::fread(&c, 1, 1, f), size_t(1));
  ASSERT_EQ(c, 'X');

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}

// Push a different character than what was read.
TEST_F(LlvmLibcWindowsUngetcTest, UngetDifferentChar) {
  constexpr const char *PATH = "ungetc_diff.tmp";
  ::FILE *f = LIBC_NAMESPACE::fopen(PATH, "w+");
  ASSERT_NE(f, static_cast<::FILE *>(nullptr));

  constexpr char DATA[] = "XY";
  ASSERT_EQ(LIBC_NAMESPACE::fwrite(DATA, 1, 2, f), size_t(2));

  LIBC_NAMESPACE::rewind(f);

  char c;
  ASSERT_EQ(LIBC_NAMESPACE::fread(&c, 1, 1, f), size_t(1));
  ASSERT_EQ(c, 'X');

  // Push back 'A' instead of the original 'X'.
  ASSERT_EQ(LIBC_NAMESPACE::ungetc('A', f), int('A'));

  char buf[3] = {};
  ASSERT_EQ(LIBC_NAMESPACE::fread(buf, 1, 2, f), size_t(2));
  ASSERT_STREQ(buf, "AY");

  ASSERT_EQ(LIBC_NAMESPACE::fclose(f), 0);
  LIBC_NAMESPACE::remove(PATH);
}
