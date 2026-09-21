/*===---- crtdbg.h - CRT debug heap wrapper --------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CRTDBG_H
#define __CLANG_CRTDBG_H

#if defined(__LLVM_LIBC__)
/* Itanium + llvm-libc: no UCRT debug heap.  Provide no-op stubs so code
   that conditionally uses _CrtSetReportMode etc. compiles cleanly. */

#define _CRT_WARN   0
#define _CRT_ERROR  1
#define _CRT_ASSERT 2

#define _CRTDBG_MODE_DEBUG   1
#define _CRTDBG_MODE_FILE    2
#define _CRTDBG_MODE_WNDW    4
#define _CRTDBG_REPORT_MODE  (-1)

#define _CRTDBG_FILE_STDERR  ((_HFILE)-2)
#define _CRTDBG_INVALID_HFILE ((_HFILE)-1)

typedef void *_HFILE;

static __inline int _CrtSetReportMode(int type, int mode) {
  (void)type; (void)mode;
  return 0;
}

static __inline _HFILE _CrtSetReportFile(int type, _HFILE file) {
  (void)type; (void)file;
  return 0;
}

/* _set_error_mode / _OUT_TO_STDERR: UCRT error routing.
   With llvm-libc, errors already go to stderr. */
#define _OUT_TO_STDERR 1
static __inline int _set_error_mode(int mode) {
  (void)mode;
  return _OUT_TO_STDERR;
}

#elif __has_include_next(<crtdbg.h>)
#include_next <crtdbg.h>
#endif

#endif /* __CLANG_CRTDBG_H */
