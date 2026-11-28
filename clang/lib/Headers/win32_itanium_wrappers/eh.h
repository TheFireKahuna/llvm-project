/*===---- eh.h - Structured exception translation --------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The part of the Visual C++ runtime's eh.h that the C++ runtime implements on
 * Windows Itanium. A thread's translator is called for a structured exception
 * that reaches a handler of a function compiled with -fasync-exceptions, while
 * the exception's handler is searched for, and makes a C++ exception of it by
 * throwing one. */

#ifndef __CLANG_EH_H
#define __CLANG_EH_H

#ifndef __cplusplus
#error "eh.h is only for C++"
#endif

struct _EXCEPTION_POINTERS;

extern "C" {
typedef void(__cdecl *_se_translator_function)(unsigned int,
                                               struct _EXCEPTION_POINTERS *);
_se_translator_function __cdecl
_set_se_translator(_se_translator_function _NewSETranslator);
}

#endif /* __CLANG_EH_H */
