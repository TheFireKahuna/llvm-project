//===--- A platform independent Dir class ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_FILE_DIR_H
#define LLVM_LIBC_SRC___SUPPORT_FILE_DIR_H

#include "src/__support/CPP/mutex.h"
#include "src/__support/CPP/span.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#include <dirent.h>

namespace LIBC_NAMESPACE_DECL {

// Platform specific function which will open the directory |name|
// and return its file descriptor. Upon failure, the error value is returned.
ErrorOr<int> platform_opendir(const char *name);

// Platform specific function which will close the directory with
// file descriptor |fd|. Returns 0 on success, or the error number on failure.
int platform_closedir(int fd);

// Platform specific function which will fetch dirents in to buffer.
// Returns the number of bytes written into buffer or the error number on
// failure. If |restart| is true, the scan restarts from the beginning.
// |last_cookie| is the d_off of the last entry returned to the user (0 at
// start). On platforms that synthesize d_off (Windows), each packed entry
// gets d_off = last_cookie + 1, +2, etc. Linux ignores this (kernel sets
// d_off via getdents64).
ErrorOr<size_t> platform_fetch_dirents(int fd, cpp::span<uint8_t> buffer,
                                       bool restart, long last_cookie);

// Platform specific function which validates that |fd| refers to a directory.
// Returns 0 on success, or the error number on failure.
int platform_fdopendir(int fd);

// Platform specific function to seek the directory stream to a position
// previously returned by telldir. The value 0 means "rewind to beginning".
// Returns true if the next fetch should restart the scan (Windows: always,
// since NT lacks directory position cookies). Returns false if the platform
// handled positioning directly (Linux: lseek).
// On platforms that return true, platform_fetch_dirents must skip entries
// when restart=true and last_cookie > 0.
bool platform_seekdir(int fd, long cookie);

// This class is designed to allow implementation of the POSIX dirent.h API.
// By itself, it is platform independent but calls platform specific
// functions to perform OS operations.
class Dir {
public:
  static constexpr size_t DEFAULT_BUFSIZE = 8192;

private:
  int fd;
  size_t readptr = 0;  // The current read pointer.
  size_t fillsize = 0; // The number of valid bytes available in the buffer.

  uint8_t *buffer;
  size_t bufsize;

  // Opaque position cookie from the last dirent returned by read().
  // On Linux, this is the kernel's d_off (usable with lseek for O(1) seek).
  // On Windows, this is a monotonic counter (seekdir requires O(n) restart).
  long last_cookie = 0;

  // When true, the next fetch restarts the directory scan from the beginning.
  bool restart_scan = true;

  Mutex mutex;

  // A directory is to be opened by the static method open and closed
  // by the close method. So, all constructors and destructor are declared
  // as private. Inappropriate constructors are declared as deleted.
  LIBC_INLINE Dir() = delete;
  LIBC_INLINE Dir(const Dir &) = delete;

  LIBC_INLINE explicit Dir(int fdesc, uint8_t *buf, size_t bufsz)
      : fd(fdesc), readptr(0), fillsize(0), buffer(buf), bufsize(bufsz),
        last_cookie(0), restart_scan(true),
        mutex(/*timed=*/false, /*recursive=*/false, /*robust=*/false,
              /*pshared=*/false) {}
  LIBC_INLINE ~Dir() = default;

  LIBC_INLINE Dir &operator=(const Dir &) = delete;

public:
  static ErrorOr<Dir *> open(const char *path);

  // Create a Dir from an existing file descriptor. The fd must refer to a
  // directory. After this call, the fd is owned by the Dir object and will
  // be closed by close().
  static ErrorOr<Dir *> from_fd(int fd);

  ErrorOr<struct ::dirent *> read();

  // Returns 0 on success or the error number on failure. If an error number
  // was returned, then the resources associated with the directory are not
  // cleaned up.
  int close();

  // Reset directory scan to the beginning.
  void rewind();

  // Return the position cookie of the last entry returned by read().
  // The value is opaque and only meaningful when passed to seek().
  LIBC_INLINE long tell() {
    cpp::lock_guard lock(mutex);
    return last_cookie;
  }

  // Seek to a position previously returned by tell().
  void seek(long loc);

  LIBC_INLINE int getfd() { return fd; }

private:
  // Allocate a Dir object with an attached buffer in a single allocation.
  static ErrorOr<Dir *> alloc_dir(int fd);
  // Free a Dir object allocated by alloc_dir.
  static void free_dir(Dir *dir);
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_FILE_DIR_H
