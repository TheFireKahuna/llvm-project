//===--- Implementation of a platform independent Dir data structure ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "dir.h"

#include "src/__support/CPP/mutex.h" // lock_guard
#include "src/__support/CPP/new.h"
#include "src/__support/alloc-checker.h"
#include "src/__support/error_or.h"
#include "src/__support/libc_errno.h" // For error macros
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

ErrorOr<Dir *> Dir::alloc_dir(int fd) {
  // Single allocation for Dir + buffer. Align Dir properly.
  constexpr size_t dir_size =
      (sizeof(Dir) + alignof(Dir) - 1) & ~(alignof(Dir) - 1);
  constexpr size_t total = dir_size + DEFAULT_BUFSIZE;

  LIBC_NAMESPACE::AllocChecker ac;
  auto *mem = new (ac) uint8_t[total];
  if (!ac)
    return LIBC_NAMESPACE::Error(ENOMEM);

  uint8_t *buf = mem + dir_size;
  Dir *dir = new (mem) Dir(fd, buf, DEFAULT_BUFSIZE);
  return dir;
}

void Dir::free_dir(Dir *dir) {
  dir->~Dir();
  delete[] reinterpret_cast<uint8_t *>(dir);
}

ErrorOr<Dir *> Dir::open(const char *path) {
  auto fd = platform_opendir(path);
  if (!fd)
    return LIBC_NAMESPACE::Error(fd.error());

  auto dir = alloc_dir(fd.value());
  if (!dir) {
    platform_closedir(fd.value());
    return dir;
  }
  return dir;
}

ErrorOr<Dir *> Dir::from_fd(int fd) {
  int err = platform_fdopendir(fd);
  if (err != 0)
    return LIBC_NAMESPACE::Error(err);

  return alloc_dir(fd);
}

ErrorOr<struct ::dirent *> Dir::read() {
  cpp::lock_guard lock(mutex);
  if (readptr >= fillsize) {
    auto readsize =
        platform_fetch_dirents(fd, {buffer, bufsize}, restart_scan,
                               last_cookie);
    restart_scan = false;
    if (!readsize)
      return LIBC_NAMESPACE::Error(readsize.error());
    fillsize = readsize.value();
    readptr = 0;
  }
  if (fillsize == 0)
    return nullptr;

  struct ::dirent *d = reinterpret_cast<struct ::dirent *>(buffer + readptr);
  readptr += d->d_reclen;
  last_cookie = static_cast<long>(d->d_off);
  return d;
}

void Dir::rewind() {
  cpp::lock_guard lock(mutex);
  readptr = 0;
  fillsize = 0;
  last_cookie = 0;
  restart_scan = true;
}

void Dir::seek(long loc) {
  if (loc == 0) {
    rewind();
    return;
  }
  cpp::lock_guard lock(mutex);
  readptr = 0;
  fillsize = 0;
  last_cookie = loc;
  // Linux: lseek positions directly (O(1)), returns false → no restart needed.
  // Windows: no-op, returns true → next fetch restarts and skips to cookie.
  restart_scan = platform_seekdir(fd, loc);
}

int Dir::close() {
  {
    cpp::lock_guard lock(mutex);
    int retval = platform_closedir(fd);
    if (retval != 0)
      return retval;
  }
  free_dir(this);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
