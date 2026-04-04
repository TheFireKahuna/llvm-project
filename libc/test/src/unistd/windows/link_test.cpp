//===-- Windows unittests for link/linkat ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - link creates a hard link; both names refer to the same file
//   - the link count visible via fstat increases by 1
//   - link to an existing destination returns EEXIST
//   - link from a nonexistent source returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/fstat.h"
#include "src/unistd/close.h"
#include "src/unistd/link.h"
#include "src/unistd/unlink.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsLinkTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// link must create a second directory entry pointing to the same inode.
// On NTFS the hard-link count (st_nlink) reflects both names.
TEST_F(LlvmLibcWindowsLinkTest, CreateHardLink) {
  constexpr const char *SRC = "link_src.tmp";
  constexpr const char *DST = "link_dst.tmp";

  int fd = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::write(fd, "data", 4);
  LIBC_NAMESPACE::close(fd);

  ASSERT_THAT(LIBC_NAMESPACE::link(SRC, DST), Succeeds(0));

  // Both names must exist; nlink must be at least 2.
  int fd2 = LIBC_NAMESPACE::open(DST, O_RDONLY);
  EXPECT_GT(fd2, 0);
  if (fd2 > 0) {
    struct stat st;
    LIBC_NAMESPACE::fstat(fd2, &st);
    EXPECT_GE(st.st_nlink, (nlink_t)2);
    LIBC_NAMESPACE::close(fd2);
  }

  LIBC_NAMESPACE::unlink(DST);
  LIBC_NAMESPACE::unlink(SRC);
}

// link to an existing destination must fail.
TEST_F(LlvmLibcWindowsLinkTest, ExistingDestination) {
  constexpr const char *SRC = "link_exist_src.tmp";
  constexpr const char *DST = "link_exist_dst.tmp";

  int fd1 = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd1, 0);
  LIBC_NAMESPACE::close(fd1);

  int fd2 = LIBC_NAMESPACE::open(DST, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd2, 0);
  LIBC_NAMESPACE::close(fd2);

  EXPECT_THAT(LIBC_NAMESPACE::link(SRC, DST), Fails(EEXIST));

  LIBC_NAMESPACE::unlink(SRC);
  LIBC_NAMESPACE::unlink(DST);
}

// link from a nonexistent source must fail with ENOENT.
TEST_F(LlvmLibcWindowsLinkTest, NonexistentSource) {
  EXPECT_THAT(LIBC_NAMESPACE::link("__no_such_src_xyz__.tmp", "link_out.tmp"),
              Fails(ENOENT));
}
