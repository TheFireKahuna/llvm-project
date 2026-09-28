/*===---- setjmp.h - Non-local jumps ---------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The UCRT implements setjmp and longjmp but leaves their declarations to the
 * Visual C++ runtime's setjmp.h. The jump buffer has that header's layout:
 * longjmp unwinds to the buffer's frame, so the frames it leaves run their
 * structured exception handlers but not C++ destructors. */

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

typedef struct _JUMP_BUFFER {
  unsigned __int64 Frame;
  unsigned __int64 Rbx;
  unsigned __int64 Rsp;
  unsigned __int64 Rbp;
  unsigned __int64 Rsi;
  unsigned __int64 Rdi;
  unsigned __int64 R12;
  unsigned __int64 R13;
  unsigned __int64 R14;
  unsigned __int64 R15;
  unsigned __int64 Rip;
  unsigned long MxCsr;
  unsigned short FpCsr;
  unsigned short Spare;
  SETJMP_FLOAT128 Xmm6;
  SETJMP_FLOAT128 Xmm7;
  SETJMP_FLOAT128 Xmm8;
  SETJMP_FLOAT128 Xmm9;
  SETJMP_FLOAT128 Xmm10;
  SETJMP_FLOAT128 Xmm11;
  SETJMP_FLOAT128 Xmm12;
  SETJMP_FLOAT128 Xmm13;
  SETJMP_FLOAT128 Xmm14;
  SETJMP_FLOAT128 Xmm15;
} _JUMP_BUFFER;

#elif defined(__aarch64__)

#define _JBLEN 24
#define _JBTYPE unsigned __int64

typedef struct _JUMP_BUFFER {
  unsigned __int64 Frame;
  unsigned __int64 Reserved;
  unsigned __int64 X19;
  unsigned __int64 X20;
  unsigned __int64 X21;
  unsigned __int64 X22;
  unsigned __int64 X23;
  unsigned __int64 X24;
  unsigned __int64 X25;
  unsigned __int64 X26;
  unsigned __int64 X27;
  unsigned __int64 X28;
  unsigned __int64 Fp;
  unsigned __int64 Lr;
  unsigned __int64 Sp;
  unsigned int Fpcr;
  unsigned int Fpsr;
  double D[8];
} _JUMP_BUFFER;

#else
#error "setjmp.h does not support this architecture"
#endif

typedef _JBTYPE jmp_buf[_JBLEN];

/* Clang calls the UCRT's implementation for _setjmp, passing it the frame that
 * longjmp unwinds to. */
int __cdecl _setjmp(jmp_buf _Buf);
#define setjmp _setjmp

__declspec(noreturn) void __cdecl longjmp(jmp_buf _Buf, int _Value);

_CRT_END_C_HEADER

#endif /* __CLANG_SETJMP_H */
