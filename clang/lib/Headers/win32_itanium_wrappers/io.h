/*===---- io.h - Low-level I/O wrapper -------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_IO_H
#define __CLANG_IO_H

#if __STDC_HOSTED__ && __has_include_next(<io.h>)
#include_next <io.h>
#endif

/*
 * Map the POSIX names to the UCRT's underscore-prefixed functions. The UCRT
 * declares the POSIX names only when __STDC__ is 0, and clang defines it as 1.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
/* These names are safe as macros - unlikely to conflict with C++ identifiers */
#  ifndef chmod
#    define chmod _chmod
#  endif
#  ifndef chsize
#    define chsize _chsize
#  endif
#  ifndef creat
#    define creat _creat
#  endif
#  ifndef dup
#    define dup _dup
#  endif
#  ifndef dup2
#    define dup2 _dup2
#  endif
#  ifndef filelength
#    define filelength _filelength
#  endif
#  ifndef isatty
#    define isatty _isatty
#  endif
#  ifndef locking
#    define locking _locking
#  endif
#  ifndef lseek
#    define lseek _lseek
#  endif
#  ifndef mktemp
#    define mktemp _mktemp
#  endif
#  ifndef setmode
#    define setmode _setmode
#  endif
#  ifndef sopen
#    define sopen _sopen
#  endif
#  ifndef umask
#    define umask _umask
#  endif
#  ifndef unlink
#    define unlink _unlink
#  endif

/*
 * These names conflict with C++ identifiers (member functions, namespace
 * functions, etc.): access, close, eof, open, read, tell, write.
 * C: use macros.
 * C++: use inline wrappers (C++ distinguishes ::open() from obj.open()).
 */
#  if !defined(__cplusplus)
#    ifndef access
#      define access _access
#    endif
#    ifndef close
#      define close _close
#    endif
#    ifndef eof
#      define eof _eof
#    endif
#    ifndef open
#      define open _open
#    endif
#    ifndef read
#      define read _read
#    endif
#    ifndef tell
#      define tell _tell
#    endif
#    ifndef write
#      define write _write
#    endif
#  else
inline int access(const char* path, int mode) { return _access(path, mode); }
inline int close(int fd) { return _close(fd); }
inline int eof(int fd) { return _eof(fd); }
inline int open(const char* path, int flags) { return _open(path, flags); }
inline int open(const char* path, int flags, int mode) { return _open(path, flags, mode); }
inline int read(int fd, void* buf, size_t count) {
  return _read(fd, buf, static_cast<unsigned int>(count));
}
inline long tell(int fd) { return _tell(fd); }
inline int write(int fd, const void* buf, size_t count) {
  return _write(fd, buf, static_cast<unsigned int>(count));
}
#  endif
#endif

#endif /* __CLANG_IO_H */
