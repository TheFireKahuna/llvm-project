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

/* The UCRT defines printf, wprintf and the rest of the formatted I/O functions
 * in stdio.h, wchar.h and conio.h as _CRT_STDIO_INLINE, which is __inline
 * unless defined first, and does not export them. Outside the Microsoft C++
 * ABI a C __inline definition is only an inline definition, so a call that is
 * not inlined would refer to a function that no library defines, and those
 * that are also declared without inline would be defined in every unit. Give
 * them internal linkage in C. The __inline__ spelling is unaffected by the
 * wrappers that define __inline as static __inline. */
#if !defined(__cplusplus) && !defined(_CRT_STDIO_INLINE)
#define _CRT_STDIO_INLINE static __inline__
#endif

/* %s and %ls in the wide printf and scanf functions take char and wchar_t
 * strings, as ISO C specifies, unless the project asks for the legacy
 * behavior. */
#if !defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS) &&                                \
    !defined(_CRT_STDIO_LEGACY_WIDE_SPECIFIERS)
#define _CRT_STDIO_ISO_WIDE_SPECIFIERS
#endif

/* The stdio option functions defined here return storage that the compiler
 * runtime shares across an image, and it defines them for C, so they keep the
 * UCRT's __inline even when a wrapper that defines __inline as static __inline
 * includes this header. Under GNU89 inline semantics, before C99 or with
 * -fgnu89-inline, a plain __inline definition would be emitted in every unit;
 * extern __inline is the GNU89 spelling of an inline definition. */
#pragma push_macro("__inline")
#undef __inline
#if !defined(__cplusplus) &&                                                   \
    (defined(__GNUC_GNU_INLINE__) || !defined(__STDC_VERSION__) ||             \
     __STDC_VERSION__ < 199901L)
#define __inline extern __inline
#endif
#include_next <corecrt_stdio_config.h>
#pragma pop_macro("__inline")

#endif /* __CLANG_CORECRT_STDIO_CONFIG_H */
