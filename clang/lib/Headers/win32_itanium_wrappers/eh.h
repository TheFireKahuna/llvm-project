/*===---- eh.h - Structured exception translation --------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The part of the Visual C++ runtime's eh.h that the C++ runtime implements on
 * Windows Itanium. A translator installed on a thread is called for a
 * structured exception that reaches a C++ frame while a handler is being
 * looked for, and turns it into a C++ exception by throwing one. */

#ifndef __CLANG_EH_H
#define __CLANG_EH_H

#ifndef __cplusplus
#error "eh.h is only for C++"
#endif

#include <exception>

struct _EXCEPTION_POINTERS;

extern "C" {
typedef void(__cdecl *_se_translator_function)(unsigned int,
                                               struct _EXCEPTION_POINTERS *);
_se_translator_function __cdecl
_set_se_translator(_se_translator_function _NewSETranslator);
}

using std::get_terminate;
using std::set_terminate;
using std::terminate;
using std::terminate_handler;

#endif /* __CLANG_EH_H */
