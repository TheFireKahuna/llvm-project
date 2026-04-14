//===-- Windows unittests for faccessat/unlinkat --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   faccessat:
//   - AT_FDCWD + existing file → 0
//   - AT_FDCWD + nonexistent path → ENOENT
//   - R_OK|W_OK on a writable file → 0
//   unlinkat:
//   - AT_FDCWD + file → removes the file
//   - AT_FDCWD + directory + AT_REMOVEDIR → removes the directory
//   - nonexistent path → ENOENT
//   - file path with AT_REMOVEDIR → ENOTDIR
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/mkdir.h"
#include "src/unistd/close.h"
#include "src/unistd/faccessat.h"
#include "src/unistd/unlinkat.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h> // AT_FDCWD, F_OK, R_OK, W_OK, AT_REMOVEDIR

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsFaccessatUnlinkatTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ── faccessat ────────────────────────────────────────────────────────────────

// F_OK on an existing file must succeed.
TEST_F(LlvmLibcWindowsFaccessatUnlinkatTest, FaccessatExisting) {
  constexpr const char *PATH = "faccessat_exist.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::faccessat(AT_FDCWD, PATH, F_OK, 0), Succeeds(0));
  LIBC_NAMESPACE::unlinkat(AT_FDCWD, PATH, 0);
}

// F_OK on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsFaccessatUnlinkatTest, FaccessatNonexistent) {
  EXPECT_THAT(
      LIBC_NAMESPACE::faccessat(AT_FDCWD, "__no_such_file_abc__.tmp", F_OK, 0),
      Fails(ENOENT));
}

// R_OK|W_OK on a file we created and own must succeed.
TEST_F(LlvmLibcWindowsFaccessatUnlinkatTest, FaccessatReadWrite) {
  constexpr const char *PATH = "faccessat_rw.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_RDWR, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::faccessat(AT_FDCWD, PATH, R_OK | W_OK, 0),
              Succeeds(0));
  LIBC_NAMESPACE::unlinkat(AT_FDCWD, PATH, 0);
}

// ── unlinkat ─────────────────────────────────────────────────────────────────

// unlinkat removes a file; a subsequent faccessat must return ENOENT.
TEST_F(LlvmLibcWindowsFaccessatUnlinkatTest, UnlinkatRemovesFile) {
  constexpr const char *PATH = "unlinkat_file.tmp";
  int fd = LIBC_NAMESPACE::open(PATH, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  ASSERT_THAT(LIBC_NAMESPACE::unlinkat(AT_FDCWD, PATH, 0), Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::faccessat(AT_FDCWD, PATH, F_OK, 0),
              Fails(ENOENT));
}

// unlinkat with AT_REMOVEDIR removes an empty directory.
TEST_F(LlvmLibcWindowsFaccessatUnlinkatTest, UnlinkatRemovesDir) {
  constexpr const char *DIR = "unlinkat_dir_test";
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));

  EXPECT_THAT(LIBC_NAMESPACE::unlinkat(AT_FDCWD, DIR, AT_REMOVEDIR),
              Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::faccessat(AT_FDCWD, DIR, F_OK, 0),
              Fails(ENOENT));
}

// unlinkat on a nonexistent path must return ENOENT.
TEST_F(LlvmLibcWindowsFaccessatUnlinkatTest, UnlinkatNonexistent) {
  EXPECT_THAT(LIBC_NAMESPACE::unlinkat(AT_FDCWD, "__no_such_xyz__.tmp", 0),
              Fails(ENOENT));
}
