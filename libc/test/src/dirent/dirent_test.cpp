//===-- Unittests for functions from POSIX dirent.h -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/string_view.h"
#include "src/dirent/alphasort.h"
#include "src/dirent/closedir.h"
#include "src/dirent/dirfd.h"
#include "src/dirent/fdopendir.h"
#include "src/dirent/opendir.h"
#include "src/dirent/readdir.h"
#include "src/dirent/rewinddir.h"
#include "src/dirent/scandir.h"
#include "src/dirent/seekdir.h"
#include "src/dirent/telldir.h"
#include "src/fcntl/open.h"
#include "src/stdlib/free.h"
#include "src/unistd/close.h"

#include "hdr/fcntl_macros.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include <dirent.h>

using LlvmLibcDirentTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using string_view = LIBC_NAMESPACE::cpp::string_view;

// Count directory entries, excluding "." and "..".
static int count_entries(::DIR *dir) {
  int count = 0;
  while (struct ::dirent *d = LIBC_NAMESPACE::readdir(dir)) {
    string_view name(d->d_name);
    if (name != "." && name != "..")
      ++count;
  }
  return count;
}

TEST_F(LlvmLibcDirentTest, SimpleOpenAndRead) {
  ::DIR *dir = LIBC_NAMESPACE::opendir("testdata");
  ASSERT_TRUE(dir != nullptr);
  // The file descriptors 0, 1 and 2 are reserved for standard streams.
  // So, the file descriptor for the newly opened directory should be
  // greater than 2.
  ASSERT_GT(LIBC_NAMESPACE::dirfd(dir), 2);

  struct ::dirent *file1 = nullptr, *file2 = nullptr, *dir1 = nullptr,
                  *dir2 = nullptr;
  while (true) {
    struct ::dirent *d = LIBC_NAMESPACE::readdir(dir);
    if (d == nullptr)
      break;
    if (string_view(&d->d_name[0]) == "file1.txt")
      file1 = d;
    if (string_view(&d->d_name[0]) == "file2.txt")
      file2 = d;
    if (string_view(&d->d_name[0]) == "dir1")
      dir1 = d;
    if (string_view(&d->d_name[0]) == "dir2")
      dir2 = d;
  }

  // Verify that we don't break out of the above loop in error.
  ASSERT_ERRNO_SUCCESS();

  ASSERT_TRUE(file1 != nullptr);
  ASSERT_TRUE(file2 != nullptr);
  ASSERT_TRUE(dir1 != nullptr);
  ASSERT_TRUE(dir2 != nullptr);

  ASSERT_EQ(LIBC_NAMESPACE::closedir(dir), 0);
}

TEST_F(LlvmLibcDirentTest, OpenNonExistentDir) {
  ::DIR *dir = LIBC_NAMESPACE::opendir("___xyz123__.non_existent__");
  ASSERT_TRUE(dir == nullptr);
  ASSERT_ERRNO_EQ(ENOENT);
}

TEST_F(LlvmLibcDirentTest, OpenFile) {
  ::DIR *dir = LIBC_NAMESPACE::opendir("testdata/file1.txt");
  ASSERT_TRUE(dir == nullptr);
  ASSERT_ERRNO_EQ(ENOTDIR);
}

TEST_F(LlvmLibcDirentTest, FdOpenDir) {
  int fd = LIBC_NAMESPACE::open("testdata", O_RDONLY | O_DIRECTORY);
  ASSERT_GT(fd, 2);

  ::DIR *dir = LIBC_NAMESPACE::fdopendir(fd);
  ASSERT_TRUE(dir != nullptr);

  // Verify we can read entries through the fdopendir'd stream.
  int entries = count_entries(dir);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_GE(entries, 4); // file1.txt, file2.txt, dir1, dir2

  // closedir closes the underlying fd.
  ASSERT_EQ(LIBC_NAMESPACE::closedir(dir), 0);
}

TEST_F(LlvmLibcDirentTest, FdOpenDirNotADirectory) {
  int fd = LIBC_NAMESPACE::open("testdata/file1.txt", O_RDONLY);
  ASSERT_GT(fd, 2);

  ::DIR *dir = LIBC_NAMESPACE::fdopendir(fd);
  ASSERT_TRUE(dir == nullptr);
  ASSERT_ERRNO_EQ(ENOTDIR);

  // fd is still ours to close on failure.
  LIBC_NAMESPACE::close(fd);
}

TEST_F(LlvmLibcDirentTest, RewindDir) {
  ::DIR *dir = LIBC_NAMESPACE::opendir("testdata");
  ASSERT_TRUE(dir != nullptr);

  int first_count = count_entries(dir);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_GE(first_count, 4);

  LIBC_NAMESPACE::rewinddir(dir);

  int second_count = count_entries(dir);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_EQ(first_count, second_count);

  ASSERT_EQ(LIBC_NAMESPACE::closedir(dir), 0);
}

TEST_F(LlvmLibcDirentTest, TellAndSeekDir) {
  ::DIR *dir = LIBC_NAMESPACE::opendir("testdata");
  ASSERT_TRUE(dir != nullptr);

  // Initial position is 0.
  ASSERT_EQ(LIBC_NAMESPACE::telldir(dir), 0l);

  // Read the first entry and record position after it.
  struct ::dirent *d1 = LIBC_NAMESPACE::readdir(dir);
  ASSERT_TRUE(d1 != nullptr);
  long pos_after_first = LIBC_NAMESPACE::telldir(dir);
  ASSERT_EQ(pos_after_first, 1l);

  // Read the second entry.
  struct ::dirent *d2 = LIBC_NAMESPACE::readdir(dir);
  ASSERT_TRUE(d2 != nullptr);
  long pos_after_second = LIBC_NAMESPACE::telldir(dir);
  ASSERT_EQ(pos_after_second, 2l);

  // Save the second entry's name for comparison.
  char saved_name[256];
  string_view d2_name(d2->d_name);
  for (size_t i = 0; i < d2_name.size(); ++i)
    saved_name[i] = d2_name[i];
  saved_name[d2_name.size()] = '\0';

  // Seek back to pos_after_first — next read should return d2's entry.
  LIBC_NAMESPACE::seekdir(dir, pos_after_first);
  ASSERT_EQ(LIBC_NAMESPACE::telldir(dir), pos_after_first);

  struct ::dirent *d2_again = LIBC_NAMESPACE::readdir(dir);
  ASSERT_TRUE(d2_again != nullptr);
  ASSERT_TRUE(string_view(d2_again->d_name) == string_view(saved_name));

  ASSERT_EQ(LIBC_NAMESPACE::closedir(dir), 0);
}

TEST_F(LlvmLibcDirentTest, SeekToZero) {
  ::DIR *dir = LIBC_NAMESPACE::opendir("testdata");
  ASSERT_TRUE(dir != nullptr);

  // Read all entries.
  int total = count_entries(dir);
  ASSERT_ERRNO_SUCCESS();

  // Seek to 0 — equivalent to rewinddir.
  LIBC_NAMESPACE::seekdir(dir, 0);
  int after_seek = count_entries(dir);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_EQ(total, after_seek);

  ASSERT_EQ(LIBC_NAMESPACE::closedir(dir), 0);
}

TEST_F(LlvmLibcDirentTest, Scandir) {
  struct ::dirent **namelist = nullptr;
  int n = LIBC_NAMESPACE::scandir("testdata", &namelist, nullptr, nullptr);
  ASSERT_GE(n, 4); // At least file1.txt, file2.txt, dir1, dir2.

  // Verify all expected entries are present.
  bool found_file1 = false, found_file2 = false;
  bool found_dir1 = false, found_dir2 = false;
  for (int i = 0; i < n; ++i) {
    string_view name(namelist[i]->d_name);
    if (name == "file1.txt")
      found_file1 = true;
    if (name == "file2.txt")
      found_file2 = true;
    if (name == "dir1")
      found_dir1 = true;
    if (name == "dir2")
      found_dir2 = true;
  }
  ASSERT_TRUE(found_file1);
  ASSERT_TRUE(found_file2);
  ASSERT_TRUE(found_dir1);
  ASSERT_TRUE(found_dir2);

  for (int i = 0; i < n; ++i)
    LIBC_NAMESPACE::free(namelist[i]);
  LIBC_NAMESPACE::free(namelist);
}

TEST_F(LlvmLibcDirentTest, ScandirWithFilter) {
  // Filter: only accept entries whose name starts with "file".
  auto filter = [](const struct ::dirent *d) -> int {
    return string_view(d->d_name).starts_with("file") ? 1 : 0;
  };

  struct ::dirent **namelist = nullptr;
  int n = LIBC_NAMESPACE::scandir("testdata", &namelist, filter, nullptr);
  ASSERT_EQ(n, 2); // file1.txt, file2.txt

  for (int i = 0; i < n; ++i)
    LIBC_NAMESPACE::free(namelist[i]);
  LIBC_NAMESPACE::free(namelist);
}

TEST_F(LlvmLibcDirentTest, ScandirWithAlphasort) {
  // Filter out "." and ".." for predictable ordering.
  auto no_dots = [](const struct ::dirent *d) -> int {
    string_view name(d->d_name);
    return (name != "." && name != "..") ? 1 : 0;
  };

  struct ::dirent **namelist = nullptr;
  int n = LIBC_NAMESPACE::scandir("testdata", &namelist, no_dots,
                                  LIBC_NAMESPACE::alphasort);
  ASSERT_GE(n, 4);

  // Verify sorted order: dir1 < dir2 < file1.txt < file2.txt.
  int idx_dir1 = -1, idx_dir2 = -1, idx_file1 = -1, idx_file2 = -1;
  for (int i = 0; i < n; ++i) {
    string_view name(namelist[i]->d_name);
    if (name == "dir1")
      idx_dir1 = i;
    if (name == "dir2")
      idx_dir2 = i;
    if (name == "file1.txt")
      idx_file1 = i;
    if (name == "file2.txt")
      idx_file2 = i;
  }
  ASSERT_GE(idx_dir1, 0);
  ASSERT_GE(idx_dir2, 0);
  ASSERT_GE(idx_file1, 0);
  ASSERT_GE(idx_file2, 0);
  ASSERT_LT(idx_dir1, idx_dir2);
  ASSERT_LT(idx_dir2, idx_file1);
  ASSERT_LT(idx_file1, idx_file2);

  for (int i = 0; i < n; ++i)
    LIBC_NAMESPACE::free(namelist[i]);
  LIBC_NAMESPACE::free(namelist);
}

TEST_F(LlvmLibcDirentTest, ScandirNonExistent) {
  struct ::dirent **namelist = nullptr;
  int n = LIBC_NAMESPACE::scandir("___xyz123__.non_existent__", &namelist,
                                  nullptr, nullptr);
  ASSERT_EQ(n, -1);
  ASSERT_ERRNO_EQ(ENOENT);
}
