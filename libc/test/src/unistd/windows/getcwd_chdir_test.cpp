//===-- Windows unittests for getcwd/chdir --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - getcwd returns the current working directory as a POSIX path
//   - getcwd with a buffer smaller than needed returns ERANGE
//   - chdir changes the working directory; a subsequent getcwd reflects it
//   - chdir to a nonexistent path returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/mkdir.h"
#include "src/unistd/chdir.h"
#include "src/unistd/getcwd.h"
#include "src/unistd/rmdir.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsGetcwdChdirTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// getcwd must return a non-null POSIX-style path (forward slashes).
TEST_F(LlvmLibcWindowsGetcwdChdirTest, ReturnsPath) {
  char buf[4096] = {};
  char *result = LIBC_NAMESPACE::getcwd(buf, sizeof(buf));
  ASSERT_NE(result, static_cast<char *>(nullptr));
  // Must be the same buffer.
  EXPECT_EQ(result, buf);
  // Windows paths start with a drive letter like "C:/...".
  EXPECT_NE(buf[0], '\0');
  // No backslashes — getcwd converts to forward slashes.
  for (int i = 0; buf[i] != '\0'; ++i)
    EXPECT_NE(buf[i], '\\');
}

// getcwd with size=1 must fail with ERANGE.
TEST_F(LlvmLibcWindowsGetcwdChdirTest, BufferTooSmall) {
  char tiny[1];
  char *result = LIBC_NAMESPACE::getcwd(tiny, sizeof(tiny));
  EXPECT_EQ(result, static_cast<char *>(nullptr));
  ASSERT_ERRNO_EQ(ERANGE);
}

// chdir changes the directory; getcwd confirms it; chdir back restores it.
TEST_F(LlvmLibcWindowsGetcwdChdirTest, ChdirAndBack) {
  constexpr const char *SUBDIR = "getcwd_chdir_testdir";
  char orig[4096] = {};
  ASSERT_NE(LIBC_NAMESPACE::getcwd(orig, sizeof(orig)),
            static_cast<char *>(nullptr));

  ASSERT_THAT(LIBC_NAMESPACE::mkdir(SUBDIR, S_IRWXU), Succeeds(0));
  ASSERT_THAT(LIBC_NAMESPACE::chdir(SUBDIR), Succeeds(0));

  char after[4096] = {};
  ASSERT_NE(LIBC_NAMESPACE::getcwd(after, sizeof(after)),
            static_cast<char *>(nullptr));
  // The new cwd must differ from the original.
  EXPECT_NE(__builtin_strcmp(orig, after), 0);

  // Restore.
  ASSERT_THAT(LIBC_NAMESPACE::chdir(orig), Succeeds(0));
  ASSERT_THAT(LIBC_NAMESPACE::rmdir(SUBDIR), Succeeds(0));
}

// chdir to a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsGetcwdChdirTest, NonexistentPath) {
  EXPECT_THAT(LIBC_NAMESPACE::chdir("__no_such_dir_xyz_12345__"), Fails(ENOENT));
}
