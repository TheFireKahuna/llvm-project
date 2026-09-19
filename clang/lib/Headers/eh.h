/*===---- eh.h - Structured exception translation for C++ -----------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_eh_h
#define __clang_eh_h

#if !defined(_WIN32_ITANIUM)
/* Elsewhere the header belongs to the Visual C++ runtime. */
#if __has_include_next(<eh.h>)
#include_next <eh.h>
#endif
#else

#ifndef __cplusplus
#error "eh.h is only for C++"
#endif

/*
 * Windows Itanium has no Visual C++ runtime; this is the part of its eh.h that
 * the C++ runtime (libc++abi in c++.lib) implements. A translator installed on
 * a thread is called for a structured exception that reaches a C++ frame while
 * a handler is being looked for; it turns the exception into a C++ one by
 * throwing. Without a translator a structured exception is offered to
 * catch (...) alone.
 */

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

#endif /* _WIN32_ITANIUM */

#endif /* __clang_eh_h */
