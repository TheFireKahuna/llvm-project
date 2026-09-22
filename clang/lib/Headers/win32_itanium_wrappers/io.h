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
 * Windows Itanium with UCRT: Map POSIX function names to underscore-prefixed
 * names. UCRT only exposes open/read/write/close etc. when !__STDC__, but we
 * want __STDC__ for standards compliance.
 *
 * With llvm-libc (LIBC_FULL_BUILD), POSIX names are provided natively — skip.
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

/*
 * Windows Itanium with LLVM libc: Map UCRT underscore-prefixed names to
 * POSIX equivalents. LLVM libc provides the POSIX functions natively.
 * Third-party code (zlib, zstd, libxml2) includes <io.h> under _WIN32 and
 * calls _open/_read/_write/_lseeki64 etc.
 */
#elif defined(_WIN32_ITANIUM)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

/* Simple aliases — POSIX equivalents exist with identical signatures. */
#define _open   open
#define _close  close
#define _read   read
#define _write  write
#define _lseek  lseek
#define _access access
#define _chmod  chmod
#define _dup    dup
#define _dup2   dup2
#define _isatty isatty
#define _unlink unlink
#define _umask  umask
#define _mktemp mktemp
#define _creat  creat
#define _fileno fileno

/* _chsize/_chsize_s: truncate/extend a file.  UCRT returns errno on failure;
   ftruncate returns -1 and sets errno.  Adapt the return convention. */
#include <errno.h>
static __inline int _chsize(int fd, long size) {
  return ftruncate(fd, (off_t)size) == -1 ? errno : 0;
}
static __inline int _chsize_s(int fd, long long size) {
  return ftruncate(fd, (off_t)size) == -1 ? errno : 0;
}

/* _pipe: UCRT pipe() variant with extra args. Map to POSIX pipe(). */
static __inline int _pipe(int *pfds, unsigned int psize, int textmode) {
  (void)psize; (void)textmode;
  return pipe(pfds);
}

/* _get_osfhandle/_open_osfhandle: CRT fd ↔ Win32 HANDLE conversion.
   Provided as real functions by llvm-libc. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
intptr_t _get_osfhandle(int fd);
int _open_osfhandle(intptr_t osfhandle, int flags);
#ifdef __cplusplus
}
#endif

/* 64-bit seek — on LLP64 (Windows x64), off_t is 64-bit, so lseek is
   already _lseeki64-equivalent. */
#define _lseeki64 lseek

/* _setmode: LLVM libc fds are always binary — this is a no-op.
   Returns the previous mode (always _O_BINARY). */
#ifndef _O_BINARY
#define _O_BINARY 0x8000
#endif
#ifndef _O_TEXT
#define _O_TEXT   0x4000
#endif
static __inline int _setmode(int fd, int mode) {
  (void)fd; (void)mode;
  return _O_BINARY;
}

/* _sopen: map to open — sharing modes are not applicable with LLVM libc. */
static __inline int _sopen(const char *path, int oflag, int shflag, ...) {
  (void)shflag;
  return open(path, oflag);
}

/* _tell: lseek(fd, 0, SEEK_CUR). */
static __inline long _tell(int fd) {
  return (long)lseek(fd, 0, 1 /* SEEK_CUR */);
}

/* _wopen: wide-char open. Converts UTF-16 path to UTF-8 then calls open().
   Stack buffer for typical paths, heap fallback for long ones. */
#include <wchar.h>
static __inline int _wopen(const wchar_t *path, int oflag, ...) {
  if (!path) return -1;

  /* Measure UTF-8 length. Each UTF-16 unit expands to at most 3 UTF-8 bytes;
     surrogate pairs (2 units) produce 4 UTF-8 bytes. Over-estimate is fine. */
  int len = 0;
  for (const wchar_t *p = path; *p; ++p) len++;

  char stack_buf[260 * 3]; /* MAX_PATH * 3 covers most paths. */
  char *buf = stack_buf;
  int buf_size = (int)sizeof(stack_buf);
  int need = (len + 1) * 3;
  if (need > buf_size) {
    buf = (char *)malloc((unsigned)need);
    if (!buf) return -1;
    buf_size = need;
  }

  /* UTF-16 to UTF-8 conversion. */
  char *dst = buf;
  for (const wchar_t *s = path; *s; ++s) {
    unsigned int c = (unsigned int)*s;
    /* Surrogate pair. */
    if (c >= 0xD800 && c <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
      ++s;
    }
    if (c < 0x80) {
      *dst++ = (char)c;
    } else if (c < 0x800) {
      *dst++ = (char)(0xC0 | (c >> 6));
      *dst++ = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      *dst++ = (char)(0xE0 | (c >> 12));
      *dst++ = (char)(0x80 | ((c >> 6) & 0x3F));
      *dst++ = (char)(0x80 | (c & 0x3F));
    } else {
      *dst++ = (char)(0xF0 | (c >> 18));
      *dst++ = (char)(0x80 | ((c >> 12) & 0x3F));
      *dst++ = (char)(0x80 | ((c >> 6) & 0x3F));
      *dst++ = (char)(0x80 | (c & 0x3F));
    }
  }
  *dst = '\0';

  int fd = open(buf, oflag);
  if (buf != stack_buf) free(buf);
  return fd;
}

/* _wfopen: wide-char fopen. Same UTF-16 to UTF-8 conversion. */
static __inline FILE *_wfopen(const wchar_t *path, const wchar_t *mode) {
  if (!path || !mode) return 0;

  /* Convert path. */
  char path_buf[260 * 3];
  char *pbuf = path_buf;
  int plen = 0;
  for (const wchar_t *p = path; *p; ++p) plen++;
  int pneed = (plen + 1) * 3;
  if (pneed > (int)sizeof(path_buf)) {
    pbuf = (char *)malloc((unsigned)pneed);
    if (!pbuf) return 0;
  }
  char *dst = pbuf;
  for (const wchar_t *s = path; *s; ++s) {
    unsigned int c = (unsigned int)*s;
    if (c >= 0xD800 && c <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
      ++s;
    }
    if (c < 0x80) { *dst++ = (char)c; }
    else if (c < 0x800) { *dst++ = (char)(0xC0|(c>>6)); *dst++ = (char)(0x80|(c&0x3F)); }
    else if (c < 0x10000) { *dst++ = (char)(0xE0|(c>>12)); *dst++ = (char)(0x80|((c>>6)&0x3F)); *dst++ = (char)(0x80|(c&0x3F)); }
    else { *dst++ = (char)(0xF0|(c>>18)); *dst++ = (char)(0x80|((c>>12)&0x3F)); *dst++ = (char)(0x80|((c>>6)&0x3F)); *dst++ = (char)(0x80|(c&0x3F)); }
  }
  *dst = '\0';

  /* Convert mode (ASCII subset — "r", "w", "rb", "wb", etc.) */
  char mode_buf[8];
  int i;
  for (i = 0; mode[i] && i < 7; ++i)
    mode_buf[i] = (char)mode[i];
  mode_buf[i] = '\0';

  FILE *f = fopen(pbuf, mode_buf);
  if (pbuf != path_buf) free(pbuf);
  return f;
}

#endif /* __MSVCRT__ || _UCRT / _WIN32_ITANIUM */

#endif /* __CLANG_IO_H */
