/* ===-------- setjmp.h ---------------------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_setjmp_h
#define __clang_setjmp_h

/* Only include this if we are aiming for MSVC compatibility. */
#if !defined(_WIN32_ITANIUM)
#include_next <setjmp.h>
#else

#pragma once
#define _INC_SETJMP

#include <vcruntime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Architecture-specific jmp_buf definitions */
#if defined(_M_IX86) || defined(__i386__)

    #define _JBLEN  16
    #define _JBTYPE int

    typedef struct __JUMP_BUFFER {
        unsigned long Ebp;
        unsigned long Ebx;
        unsigned long Edi;
        unsigned long Esi;
        unsigned long Esp;
        unsigned long Eip;
        unsigned long Registration;
        unsigned long TryLevel;
        unsigned long Cookie;
        unsigned long UnwindFunc;
        unsigned long UnwindData[6];
    } _JUMP_BUFFER;

#elif defined(_M_X64) || defined(__x86_64__)

    typedef struct __attribute__((aligned(16))) _SETJMP_FLOAT128 {
        unsigned long long Part[2];
    } SETJMP_FLOAT128;

    #define _JBLEN  16
    typedef SETJMP_FLOAT128 _JBTYPE;

    typedef struct _JUMP_BUFFER {
        unsigned long long Frame;
        unsigned long long Rbx;
        unsigned long long Rsp;
        unsigned long long Rbp;
        unsigned long long Rsi;
        unsigned long long Rdi;
        unsigned long long R12;
        unsigned long long R13;
        unsigned long long R14;
        unsigned long long R15;
        unsigned long long Rip;
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

#elif defined(_M_ARM64) || defined(__aarch64__)

    #define _JBLEN  24
    #define _JBTYPE unsigned long long

    typedef struct _JUMP_BUFFER {
        unsigned long long Frame;
        unsigned long long Reserved;
        unsigned long long X19;
        unsigned long long X20;
        unsigned long long X21;
        unsigned long long X22;
        unsigned long long X23;
        unsigned long long X24;
        unsigned long long X25;
        unsigned long long X26;
        unsigned long long X27;
        unsigned long long X28;
        unsigned long long Fp;
        unsigned long long Lr;
        unsigned long long Sp;
        unsigned int Fpcr;
        unsigned int Fpsr;
        double D[8];
    } _JUMP_BUFFER;

#else
    #error "Unsupported architecture for setjmp.h"
#endif

/* Define jmp_buf */
#ifndef _JMP_BUF_DEFINED
    #define _JMP_BUF_DEFINED
    typedef _JBTYPE jmp_buf[_JBLEN];
#endif

/* Function prototypes */
/* UCRT: setjmp is aliased to _setjmp on Windows */
#ifndef _INC_SETJMPEX
    #define setjmp _setjmp
int __cdecl _setjmp(
    _Out_ jmp_buf _Buf
    );
#endif

__declspec(noreturn) void __cdecl longjmp(
    _In_reads_(_JBLEN) jmp_buf _Buf,
    _In_ int _Value
    );

#ifdef __cplusplus
}
#endif

#endif
#endif
