/*===---- setjmp.h - Non-local jumps ---------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The UCRT implements setjmp and longjmp but leaves their declarations to the
 * Visual C++ runtime's setjmp.h, whose jump buffer size and alignment these
 * keep. longjmp unwinds to the buffer's frame, so the frames it leaves run
 * their structured exception handlers but not C++ destructors. */

#ifndef __CLANG_SETJMP_H
#define __CLANG_SETJMP_H

#include <vcruntime.h>

_CRT_BEGIN_C_HEADER

#if defined(__x86_64__)

typedef struct __attribute__((aligned(16))) _SETJMP_FLOAT128 {
  unsigned __int64 Part[2];
} SETJMP_FLOAT128;

#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;

#elif defined(__aarch64__)

#define _JBLEN 24
#define _JBTYPE unsigned __int64

#else
#error "setjmp.h does not support this architecture"
#endif

typedef _JBTYPE jmp_buf[_JBLEN];

/* Clang calls the UCRT's implementation for _setjmp, passing it the frame that
 * longjmp unwinds to. */
int __cdecl _setjmp(jmp_buf _Buf);
#define setjmp _setjmp

__attribute__((__noreturn__)) void __cdecl longjmp(jmp_buf _Buf, int _Value);

_CRT_END_C_HEADER

#endif /* __CLANG_SETJMP_H */
