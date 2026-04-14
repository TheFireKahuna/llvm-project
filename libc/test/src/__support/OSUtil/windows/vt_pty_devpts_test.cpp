//===-- devpts PTY test (LLVM libc test framework) ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Verifies the /dev/pts subsystem: allocating a master PTY via /dev/ptmx,
// preparing the slave with grantpt/unlockpt/ptsname_r, and opening it via
// both absolute path and openat(pts_dir_fd, id).
//
//===----------------------------------------------------------------------===//

#include "test/UnitTest/Test.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr const char *DEV_PTS_PREFIX = "/dev/pts/";

const char *pts_basename(const char *path) {
  size_t prefix_len = __builtin_strlen(DEV_PTS_PREFIX);
  if (__builtin_strncmp(path, DEV_PTS_PREFIX, prefix_len) != 0)
    return nullptr;
  const char *base = path + prefix_len;
  return *base ? base : nullptr;
}

} // namespace

TEST(LlvmLibcVtPtyDevptsTest, OpenDevAndDevPts) {
  int dev_fd = open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(dev_fd, 0);

  int pts_dir_fd = openat(dev_fd, "pts", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(pts_dir_fd, 0);

  close(pts_dir_fd);
  close(dev_fd);
}

TEST(LlvmLibcVtPtyDevptsTest, AllocateMasterViaDevPtmx) {
  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  close(ptmx_fd);
}

TEST(LlvmLibcVtPtyDevptsTest, GrantAndUnlockPty) {
  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);

  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  close(ptmx_fd);
}

TEST(LlvmLibcVtPtyDevptsTest, PtsnameReturnsValidPath) {
  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  char slave_name[64] = {};
  ASSERT_EQ(ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);

  // Must start with /dev/pts/ and have a numeric suffix.
  const char *base = pts_basename(slave_name);
  ASSERT_NE(base, static_cast<const char *>(nullptr));
  EXPECT_GT(__builtin_strlen(base), static_cast<size_t>(0));

  close(ptmx_fd);
}

TEST(LlvmLibcVtPtyDevptsTest, OpenSlaveByAbsolutePath) {
  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  char slave_name[64] = {};
  ASSERT_EQ(ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);

  int slave_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  ASSERT_GE(slave_fd, 0);

  close(slave_fd);
  close(ptmx_fd);
}

TEST(LlvmLibcVtPtyDevptsTest, OpenSlaveViaOpenat) {
  int pts_dir_fd = open("/dev/pts", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(pts_dir_fd, 0);

  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  char slave_name[64] = {};
  ASSERT_EQ(ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)), 0);

  const char *base = pts_basename(slave_name);
  ASSERT_NE(base, static_cast<const char *>(nullptr));

  int slave_fd = openat(pts_dir_fd, base, O_RDWR | O_NOCTTY | O_CLOEXEC);
  ASSERT_GE(slave_fd, 0);

  close(slave_fd);
  close(ptmx_fd);
  close(pts_dir_fd);
}

TEST(LlvmLibcVtPtyDevptsTest, PtsnameFailsOnNonMasterFd) {
  char buf[64] = {};
  // stdin (fd 0) is not a master PTY — ptsname_r must fail.
  int rc = ptsname_r(0, buf, sizeof(buf));
  EXPECT_NE(rc, 0);
}

TEST(LlvmLibcVtPtyDevptsTest, PtsnameFailsWithSmallBuffer) {
  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  ASSERT_GE(ptmx_fd, 0);
  ASSERT_EQ(grantpt(ptmx_fd), 0);
  ASSERT_EQ(unlockpt(ptmx_fd), 0);

  char tiny[2] = {};
  int rc = ptsname_r(ptmx_fd, tiny, sizeof(tiny));
  EXPECT_NE(rc, 0);

  close(ptmx_fd);
}
