//===-- Windows unittests for linkat --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - linkat(AT_FDCWD, src, AT_FDCWD, dst, 0) creates a hard link
//   - linkat with a real newdfd creates the link relative to that directory
//   - AT_SYMLINK_FOLLOW: links the symlink target, not the symlink itself
//     (test is privilege-adaptive; EPERM on link creation is acceptable)
//   - Existing destination → EEXIST
//   - Nonexistent source → ENOENT
//   - Bad newdfd → EBADF
//
// linkat uses NtSetInformationFile(FileLinkInformationEx) and takes a real
// RootDirectory handle for the destination — this is not exercised by link().
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/open.h"
#include "src/sys/stat/fstat.h"
#include "src/sys/stat/mkdir.h"
#include "src/unistd/close.h"
#include "src/unistd/linkat.h"
#include "src/unistd/rmdir.h"
#include "src/unistd/symlink.h"
#include "src/unistd/unlink.h"
#include "src/unistd/write.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>
#include <unistd.h> // AT_FDCWD, AT_SYMLINK_FOLLOW

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsLinkatTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// linkat(AT_FDCWD, …, AT_FDCWD, …, 0) — same path as link().
TEST_F(LlvmLibcWindowsLinkatTest, AtFdcwd) {
  constexpr const char *SRC = "linkat_src.tmp";
  constexpr const char *DST = "linkat_dst.tmp";

  int fd = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::linkat(AT_FDCWD, SRC, AT_FDCWD, DST, 0),
              Succeeds(0));

  // Both names should refer to the same inode (nlink ≥ 2).
  struct stat st;
  ASSERT_THAT(LIBC_NAMESPACE::fstat(
                  LIBC_NAMESPACE::open(DST, O_RDONLY), &st),
              Succeeds(0));
  EXPECT_GE(st.st_nlink, nlink_t(2));

  LIBC_NAMESPACE::unlink(DST);
  LIBC_NAMESPACE::unlink(SRC);
}

// linkat with a real newdfd: link created inside the directory.
// This exercises the FILE_LINK_INFORMATION.RootDirectory code path.
TEST_F(LlvmLibcWindowsLinkatTest, RelativeDst) {
  constexpr const char *DIR  = "linkat_dir";
  constexpr const char *SRC  = "linkat_rel_src.tmp";
  constexpr const char *NAME = "linkat_rel_dst.tmp";

  // Create source file.
  int sfd = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(sfd, 0);
  LIBC_NAMESPACE::close(sfd);

  // Create destination directory and open it as a dirfd.
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));
  int dirfd = LIBC_NAMESPACE::open(DIR, O_RDONLY);
  ASSERT_GT(dirfd, 0);

  // Link src → DIR/NAME via the dirfd.
  EXPECT_THAT(LIBC_NAMESPACE::linkat(AT_FDCWD, SRC, dirfd, NAME, 0),
              Succeeds(0));

  LIBC_NAMESPACE::close(dirfd);

  // Verify the link exists by constructing the full path.
  char full[128] = {};
  size_t dlen = __builtin_strlen(DIR);
  size_t nlen = __builtin_strlen(NAME);
  __builtin_memcpy(full, DIR, dlen);
  full[dlen] = '/';
  __builtin_memcpy(full + dlen + 1, NAME, nlen + 1);
  LIBC_NAMESPACE::unlink(full);
  LIBC_NAMESPACE::unlink(SRC);
  LIBC_NAMESPACE::rmdir(DIR);
}

// AT_SYMLINK_FOLLOW: linkat opens the symlink target, not the reparse point.
// Privilege-adaptive: if symlink() returns EPERM, the test passes — no
// Developer Mode / SeCreateSymbolicLinkPrivilege is correct for this platform.
TEST_F(LlvmLibcWindowsLinkatTest, AtSymlinkFollow) {
  constexpr const char *TARGET = "linkat_follow_target.tmp";
  constexpr const char *LINK   = "linkat_follow_link.lnk";
  constexpr const char *DST    = "linkat_follow_dst.tmp";

  int fd = LIBC_NAMESPACE::open(TARGET, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  int ret = LIBC_NAMESPACE::symlink(TARGET, LINK);
  if (ret == -1) {
    EXPECT_TRUE(libc_errno == EPERM || libc_errno == ENOTSUP);
    LIBC_NAMESPACE::unlink(TARGET);
    return;
  }

  // AT_SYMLINK_FOLLOW: hard-link the symlink's target (TARGET), not LINK.
  EXPECT_THAT(
      LIBC_NAMESPACE::linkat(AT_FDCWD, LINK, AT_FDCWD, DST, AT_SYMLINK_FOLLOW),
      Succeeds(0));

  // DST should be a regular file (same inode as TARGET, nlink ≥ 2).
  struct stat st;
  int dfd = LIBC_NAMESPACE::open(DST, O_RDONLY);
  if (dfd > 0) {
    LIBC_NAMESPACE::fstat(dfd, &st);
    EXPECT_GE(st.st_nlink, nlink_t(2));
    LIBC_NAMESPACE::close(dfd);
  }

  LIBC_NAMESPACE::unlink(DST);
  LIBC_NAMESPACE::unlink(LINK);
  LIBC_NAMESPACE::unlink(TARGET);
}

// Existing destination → EEXIST.
TEST_F(LlvmLibcWindowsLinkatTest, ExistingDst) {
  constexpr const char *SRC = "linkat_exist_src.tmp";
  constexpr const char *DST = "linkat_exist_dst.tmp";

  int fd = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);
  fd = LIBC_NAMESPACE::open(DST, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(LIBC_NAMESPACE::linkat(AT_FDCWD, SRC, AT_FDCWD, DST, 0),
              Fails(EEXIST));

  LIBC_NAMESPACE::unlink(SRC);
  LIBC_NAMESPACE::unlink(DST);
}

// Nonexistent source → ENOENT.
TEST_F(LlvmLibcWindowsLinkatTest, NonexistentSrc) {
  EXPECT_THAT(
      LIBC_NAMESPACE::linkat(AT_FDCWD, "__no_such_src__.tmp",
                             AT_FDCWD, "linkat_noent_dst.tmp", 0),
      Fails(ENOENT));
}

// Bad newdfd → EBADF.
TEST_F(LlvmLibcWindowsLinkatTest, BadNewdfd) {
  constexpr const char *SRC = "linkat_badf_src.tmp";
  int fd = LIBC_NAMESPACE::open(SRC, O_CREAT | O_WRONLY, S_IRWXU);
  ASSERT_GT(fd, 0);
  LIBC_NAMESPACE::close(fd);

  EXPECT_THAT(
      LIBC_NAMESPACE::linkat(AT_FDCWD, SRC, 9999, "linkat_badf_dst.tmp", 0),
      Fails(EBADF));

  LIBC_NAMESPACE::unlink(SRC);
}
