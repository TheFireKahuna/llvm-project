/*===---- __stddef_wchar.h - Definition of wchar_t -------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* __is_identifier(wchar_t) is the compiler truth: 0 when wchar_t is a keyword
 * (real C++), 1 in C. The __cplusplus macro alone is not reliable here — the
 * UCRT wrapper headers hide it around #include_next to present the UCRT as a
 * plain C library, and a typedef of the wchar_t keyword is an error. */
#if (!defined(__cplusplus) && __is_identifier(wchar_t)) ||                     \
    (defined(_MSC_VER) && !_NATIVE_WCHAR_T_DEFINED)

/*
 * When -fbuiltin-headers-in-system-modules is set this is a non-modular header
 * and needs to behave as if it was textual.
 */
#if !defined(_WCHAR_T) ||                                                      \
    (__has_feature(modules) && !__building_module(_Builtin_stddef))
#define _WCHAR_T

// NTPOSIX is excluded: it uses POSIX wchar_t (32-bit, UTF-32), not Windows
// wchar_t (16-bit unsigned short). NTPOSIX wchar_t is set by TargetInfo.
#if defined(_MSC_EXTENSIONS) || defined(_WIN32_ITANIUM)
#define _WCHAR_T_DEFINED
#endif

typedef __WCHAR_TYPE__ wchar_t;

#endif

#endif
