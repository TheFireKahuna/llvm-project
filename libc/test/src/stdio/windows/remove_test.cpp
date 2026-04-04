//===-- Windows unittests for remove --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - remove on a regular file deletes it
//   - remove on a directory deletes it (empty dir)
//   - remove on a nonexistent path returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/sys/stat/mkdir.h"
#include "src/sys/stat/stat.h"
#include "src/unistd/close.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsRemoveTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// remove() on a regular file must delete it; subsequent stat must fail.
TEST_F(LlvmLibcWindowsRemoveTest, RemoveFile) {
  constexpr const char *PATH = "remove_file.tmp";

  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  ASSERT_THAT(LIBC_NAMESPACE::close(fd), Succeeds(0));

  ASSERT_THAT(LIBC_NAMESPACE::remove(PATH), Succeeds(0));

  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::stat(PATH, &buf), Fails(ENOENT));
}

// remove() on an empty directory must delete it.
TEST_F(LlvmLibcWindowsRemoveTest, RemoveDirectory) {
  constexpr const char *DIR = "remove_dir.tmp";

  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));
  ASSERT_THAT(LIBC_NAMESPACE::remove(DIR), Succeeds(0));

  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::stat(DIR, &buf), Fails(ENOENT));
}

// remove() on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsRemoveTest, NonexistentPath) {
  EXPECT_THAT(LIBC_NAMESPACE::remove("no_such_file_remove.tmp"), Fails(ENOENT));
}
