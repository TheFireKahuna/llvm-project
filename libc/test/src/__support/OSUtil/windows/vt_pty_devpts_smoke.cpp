//===-- devpts PTY smoke --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr const char *DEV_PTS_PREFIX = "/dev/pts/";

int fail(int stage) {
  fprintf(stderr, "vt_pty_devpts_smoke: stage=%d errno=%d\n", stage, errno);
  return stage;
}

const char *pts_basename(const char *path) {
  size_t prefix_len = __builtin_strlen(DEV_PTS_PREFIX);
  if (__builtin_strncmp(path, DEV_PTS_PREFIX, prefix_len) != 0)
    return nullptr;
  const char *base = path + prefix_len;
  return *base ? base : nullptr;
}

} // namespace

extern "C" int main() {
  int dev_fd = open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dev_fd < 0)
    return fail(10);

  int pts_dir_fd = openat(dev_fd, "pts", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (pts_dir_fd < 0) {
    close(dev_fd);
    return fail(11);
  }

  int ptmx_fd = open("/dev/ptmx", O_RDWR | O_CLOEXEC);
  if (ptmx_fd < 0) {
    close(pts_dir_fd);
    close(dev_fd);
    return fail(12);
  }
  fprintf(stderr, "ptmx_fd=%d\n", ptmx_fd);

  if (grantpt(ptmx_fd) != 0) {
    int err = errno;
    close(ptmx_fd);
    close(pts_dir_fd);
    close(dev_fd);
    errno = err;
    return fail(13);
  }

  if (unlockpt(ptmx_fd) != 0) {
    int err = errno;
    close(ptmx_fd);
    close(pts_dir_fd);
    close(dev_fd);
    errno = err;
    return fail(14);
  }

  char slave_name[64] = {};
  if (ptsname_r(ptmx_fd, slave_name, sizeof(slave_name)) != 0) {
    int err = errno;
    close(ptmx_fd);
    close(pts_dir_fd);
    close(dev_fd);
    errno = err;
    return fail(15);
  }

  int slave_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (slave_fd < 0) {
    close(ptmx_fd);
    close(pts_dir_fd);
    close(dev_fd);
    return fail(16);
  }

  const char *base = pts_basename(slave_name);
  if (!base) {
    close(slave_fd);
    close(ptmx_fd);
    close(pts_dir_fd);
    close(dev_fd);
    return fail(17);
  }

  int slave_fd_at =
      openat(pts_dir_fd, base, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (slave_fd_at < 0) {
    close(slave_fd);
    close(ptmx_fd);
    close(pts_dir_fd);
    close(dev_fd);
    return fail(18);
  }

  close(slave_fd_at);
  close(slave_fd);
  close(ptmx_fd);
  close(pts_dir_fd);
  close(dev_fd);
  return 0;
}
