/*===---- corecrt_stdio_config.h - UCRT stdio configuration wrapper --------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CORECRT_STDIO_CONFIG_H
#define __CLANG_CORECRT_STDIO_CONFIG_H

/* %s and %ls in the wide printf and scanf functions take char and wchar_t
 * strings, as ISO C specifies, unless the project asks for the legacy
 * behavior. */
#if !defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS) &&                                \
    !defined(_CRT_STDIO_LEGACY_WIDE_SPECIFIERS)
#define _CRT_STDIO_ISO_WIDE_SPECIFIERS
#endif

/* The UCRT defines printf, scanf and the rest of its formatted I/O inline in
 * these headers, and no DLL of it exports them. The compiler runtime defines
 * and exports them, and they are declared imported from it, so that a program
 * has one definition of each, as on other targets. A translation unit that
 * defines _CRT_STDIO_INLINE itself keeps the inline definitions. */
#if !defined(_CRT_STDIO_INLINE)
#define __CLANG_UCRT_STDIO_IMPORT
#ifndef _NO_CRT_STDIO_INLINE
#define _NO_CRT_STDIO_INLINE
#endif
#endif

#include_next <corecrt_stdio_config.h>

#ifdef __CLANG_UCRT_STDIO_IMPORT
#undef __CLANG_UCRT_STDIO_IMPORT
#undef _CRT_STDIO_INLINE
#define _CRT_STDIO_INLINE __declspec(dllimport)
#endif

#endif /* __CLANG_CORECRT_STDIO_CONFIG_H */
