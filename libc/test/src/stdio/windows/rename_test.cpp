//===-- Windows unittests for rename --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - rename moves a file to a new name
//   - old name no longer exists after rename
//   - rename replaces an existing destination atomically
//   - rename of a nonexistent source returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/stdio/remove.h"
#include "src/stdio/rename.h"
#include "src/sys/stat/stat.h"
#include "src/unistd/close.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsRenameTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

static void create_file(const char *path) {
  int fd = LIBC_NAMESPACE::open(path, O_CREAT | O_WRONLY, S_IRWXU);
  LIBC_NAMESPACE::close(fd);
}

// rename moves src to dst; src disappears and dst appears.
TEST_F(LlvmLibcWindowsRenameTest, MoveFile) {
  constexpr const char *SRC = "rename_src.tmp";
  constexpr const char *DST = "rename_dst.tmp";

  create_file(SRC);

  ASSERT_THAT(LIBC_NAMESPACE::rename(SRC, DST), Succeeds(0));

  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::stat(SRC, &buf), Fails(ENOENT));
  EXPECT_THAT(LIBC_NAMESPACE::stat(DST, &buf), Succeeds(0));

  LIBC_NAMESPACE::rename(DST, SRC); // best-effort cleanup
  LIBC_NAMESPACE::remove(SRC);
}

// rename with POSIX semantics atomically replaces an existing destination.
TEST_F(LlvmLibcWindowsRenameTest, AtomicReplace) {
  constexpr const char *SRC = "rename_replace_src.tmp";
  constexpr const char *DST = "rename_replace_dst.tmp";

  create_file(SRC);
  create_file(DST); // both exist

  // Must succeed even though DST already exists.
  EXPECT_THAT(LIBC_NAMESPACE::rename(SRC, DST), Succeeds(0));

  // SRC is gone; DST still exists.
  struct stat buf;
  EXPECT_THAT(LIBC_NAMESPACE::stat(SRC, &buf), Fails(ENOENT));
  EXPECT_THAT(LIBC_NAMESPACE::stat(DST, &buf), Succeeds(0));

  LIBC_NAMESPACE::remove(DST);
}

// rename of a nonexistent source must return ENOENT.
TEST_F(LlvmLibcWindowsRenameTest, NonexistentSource) {
  EXPECT_THAT(LIBC_NAMESPACE::rename("no_such_src.tmp", "no_such_dst.tmp"),
              Fails(ENOENT));
}
