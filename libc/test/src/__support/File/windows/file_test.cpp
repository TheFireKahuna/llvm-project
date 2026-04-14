//===-- Unittests for the Windows File implementation ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/file.h"
#include "test/UnitTest/Test.h"

#include "hdr/stdio_macros.h"

using File = LIBC_NAMESPACE::File;

LIBC_INLINE File *openfile(const char *file_name, const char *mode) {
  auto r = LIBC_NAMESPACE::openfile(file_name, mode);
  return r.has_value() ? r.value() : nullptr;
}

TEST(LlvmLibcWindowsFileTest, CreateWriteCloseAndReadBack) {
  constexpr char FILENAME[] =
      APPEND_LIBC_TEST("testdata/create_write_close_readback.test");
  constexpr char TEXT[] = "Hello, WindowsFile";
  constexpr size_t TEXT_SIZE = sizeof(TEXT) - 1;

  File *f = openfile(FILENAME, "w");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->write(TEXT, TEXT_SIZE).value, TEXT_SIZE);
  ASSERT_EQ(f->close(), 0);

  f = openfile(FILENAME, "r");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  char data[sizeof(TEXT)] = {};
  ASSERT_EQ(f->read(data, TEXT_SIZE).value, TEXT_SIZE);
  ASSERT_STREQ(data, TEXT);

  // EOF sentinel.
  ASSERT_EQ(f->read(data, 1).value, size_t(0));
  ASSERT_TRUE(f->iseof());
  ASSERT_EQ(f->close(), 0);
}

TEST(LlvmLibcWindowsFileTest, SeekSetCurEnd) {
  constexpr char FILENAME[] =
      APPEND_LIBC_TEST("testdata/seek_set_cur_end.test");
  constexpr char CONTENT[] = "ABCDEFGHIJ";
  constexpr size_t LEN = sizeof(CONTENT) - 1;

  File *f = openfile(FILENAME, "w");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->write(CONTENT, LEN).value, LEN);
  ASSERT_EQ(f->close(), 0);

  f = openfile(FILENAME, "r");
  ASSERT_NE(f, static_cast<File *>(nullptr));

  char buf[8] = {};
  // SEEK_SET to byte 3.
  ASSERT_EQ(f->seek(3, SEEK_SET).value(), 0);
  ASSERT_EQ(f->read(buf, 2).value, size_t(2));
  ASSERT_EQ(buf[0], 'D');
  ASSERT_EQ(buf[1], 'E');

  // SEEK_CUR +2, skip FG, read HI.
  ASSERT_EQ(f->seek(2, SEEK_CUR).value(), 0);
  ASSERT_EQ(f->read(buf, 2).value, size_t(2));
  ASSERT_EQ(buf[0], 'H');
  ASSERT_EQ(buf[1], 'I');

  // SEEK_END -3, read HIJ.
  ASSERT_EQ(f->seek(-3, SEEK_END).value(), 0);
  ASSERT_EQ(f->read(buf, 3).value, size_t(3));
  ASSERT_EQ(buf[0], 'H');
  ASSERT_EQ(buf[1], 'I');
  ASSERT_EQ(buf[2], 'J');

  ASSERT_EQ(f->close(), 0);
}

TEST(LlvmLibcWindowsFileTest, AppendMode) {
  constexpr char FILENAME[] = APPEND_LIBC_TEST("testdata/append_mode.test");

  File *f = openfile(FILENAME, "w");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->write("Hello", 5).value, size_t(5));
  ASSERT_EQ(f->close(), 0);

  f = openfile(FILENAME, "a");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->write(", World", 7).value, size_t(7));
  ASSERT_EQ(f->close(), 0);

  f = openfile(FILENAME, "r");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  char data[13] = {};
  ASSERT_EQ(f->read(data, 12).value, size_t(12));
  ASSERT_STREQ(data, "Hello, World");
  ASSERT_EQ(f->close(), 0);
}

TEST(LlvmLibcWindowsFileTest, OpenNonexistentForRead) {
  // openfile must return nullptr for a path that does not exist.
  File *f = openfile(APPEND_LIBC_TEST("testdata/does_not_exist_xyzzy.test"),
                     "r");
  EXPECT_EQ(f, static_cast<File *>(nullptr));
}

TEST(LlvmLibcWindowsFileTest, LargeFile) {
  constexpr char FILENAME[] = APPEND_LIBC_TEST("testdata/large_file.test");
  constexpr size_t SIZE = File::DEFAULT_BUFFER_SIZE * 3;
  constexpr char BYTE = 42;

  File *f = openfile(FILENAME, "w");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  // Write in 512-byte chunks to avoid a large stack allocation.
  char chunk[512];
  for (size_t i = 0; i < sizeof(chunk); ++i)
    chunk[i] = BYTE;
  for (size_t written = 0; written < SIZE; written += sizeof(chunk))
    ASSERT_EQ(f->write(chunk, sizeof(chunk)).value, sizeof(chunk));
  ASSERT_EQ(f->close(), 0);

  f = openfile(FILENAME, "r");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  size_t total = 0;
  while (total < SIZE) {
    size_t n = SIZE - total < sizeof(chunk) ? SIZE - total : sizeof(chunk);
    ASSERT_EQ(f->read(chunk, n).value, n);
    for (size_t i = 0; i < n; ++i)
      ASSERT_EQ(chunk[i], BYTE);
    total += n;
  }
  ASSERT_EQ(f->read(chunk, 1).value, size_t(0));
  ASSERT_TRUE(f->iseof());
  ASSERT_EQ(f->close(), 0);
}

TEST(LlvmLibcWindowsFileTest, WritePlusMode) {
  constexpr char FILENAME[] =
      APPEND_LIBC_TEST("testdata/write_plus_mode.test");

  File *f = openfile(FILENAME, "w+");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->write("XYZ", 3).value, size_t(3));
  ASSERT_EQ(f->seek(0, SEEK_SET).value(), 0);
  char buf[4] = {};
  ASSERT_EQ(f->read(buf, 3).value, size_t(3));
  ASSERT_STREQ(buf, "XYZ");
  ASSERT_EQ(f->close(), 0);
}

TEST(LlvmLibcWindowsFileTest, IncorrectOperation) {
  constexpr char FILENAME[] =
      APPEND_LIBC_TEST("testdata/incorrect_operation.test");
  char data[1] = {0};

  // Write-only: read should fail without advancing EOF.
  File *f = openfile(FILENAME, "w");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->read(data, 1).value, size_t(0));
  ASSERT_FALSE(f->iseof());
  ASSERT_TRUE(f->error());
  ASSERT_EQ(f->close(), 0);

  // Read-only: write should fail.
  f = openfile(FILENAME, "r");
  ASSERT_NE(f, static_cast<File *>(nullptr));
  ASSERT_EQ(f->write(data, 1).value, size_t(0));
  ASSERT_FALSE(f->iseof());
  ASSERT_TRUE(f->error());
  ASSERT_EQ(f->close(), 0);
}
