//===--- Linux implementation of the Dir helpers --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/dir.h"

#include "src/__support/OSUtil/syscall.h" // For internal syscall function.
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "hdr/fcntl_macros.h" // For open flags
#include <sys/syscall.h> // For syscall numbers

namespace LIBC_NAMESPACE_DECL {

ErrorOr<int> platform_opendir(const char *name) {
  int open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef SYS_open
  int fd = LIBC_NAMESPACE::syscall_impl<int>(SYS_open, name, open_flags);
#elif defined(SYS_openat)
  int fd =
      LIBC_NAMESPACE::syscall_impl<int>(SYS_openat, AT_FDCWD, name, open_flags);
#else
#error                                                                         \
    "SYS_open and SYS_openat syscalls not available to perform an open operation."
#endif

  if (fd < 0) {
    return LIBC_NAMESPACE::Error(-fd);
  }
  return fd;
}

ErrorOr<size_t> platform_fetch_dirents(int fd, cpp::span<uint8_t> buffer,
                                       bool restart,
                                       long /*last_cookie*/) {
  if (restart) {
    // Reset the directory cursor to the beginning.
    LIBC_NAMESPACE::syscall_impl<long>(SYS_lseek, fd, 0, SEEK_SET);
  }

#ifdef SYS_getdents64
  long size = LIBC_NAMESPACE::syscall_impl<long>(SYS_getdents64, fd,
                                                 buffer.data(), buffer.size());
#else
#error "getdents64 syscalls not available to perform a fetch dirents operation."
#endif

  if (size < 0) {
    return LIBC_NAMESPACE::Error(static_cast<int>(-size));
  }
  return size;
}

int platform_closedir(int fd) {
  int ret = LIBC_NAMESPACE::syscall_impl<int>(SYS_close, fd);
  if (ret < 0) {
    return static_cast<int>(-ret);
  }
  return 0;
}

int platform_fdopendir(int fd) {
  // Validate that fd refers to a directory via fstat.
  struct stat st;
#ifdef SYS_fstat
  int ret = LIBC_NAMESPACE::syscall_impl<int>(SYS_fstat, fd, &st);
#elif defined(SYS_fstatat)
  int ret = LIBC_NAMESPACE::syscall_impl<int>(
      SYS_fstatat, fd, "", &st, AT_EMPTY_PATH);
#elif defined(SYS_newfstatat)
  int ret = LIBC_NAMESPACE::syscall_impl<int>(
      SYS_newfstatat, fd, "", &st, AT_EMPTY_PATH);
#else
#error "No fstat syscall available."
#endif
  if (ret < 0)
    return static_cast<int>(-ret);
  if (!S_ISDIR(st.st_mode))
    return ENOTDIR;
  return 0;
}

bool platform_seekdir(int fd, long cookie) {
  // The cookie is the kernel's d_off value. lseek positions the directory
  // stream directly — O(1). Cookie 0 rewinds to the beginning.
  LIBC_NAMESPACE::syscall_impl<long>(SYS_lseek, fd, cookie, SEEK_SET);
  return false; // No restart needed — kernel is positioned.
}

} // namespace LIBC_NAMESPACE_DECL
