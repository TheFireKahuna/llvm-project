/*===---- __stddef_null.h - Definition of NULL -----------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#if !defined(NULL) || !__building_module(_Builtin_stddef)

/* linux/stddef.h will define NULL to 0. glibc (and other) headers then define
 * __need_NULL and rely on stddef.h to redefine NULL to the correct value again.
 * Modules don't support redefining macros like that, but support that pattern
 * in the non-modules case.
 */
#undef NULL

/* __is_identifier(wchar_t) is the compiler truth: 0 when wchar_t is a keyword
 * (real C++), 1 in C. The __cplusplus macro alone is not reliable here — the
 * UCRT wrapper headers hide it around #include_next to present the UCRT as a
 * plain C library, and a ((void*)0) NULL leaking out of such a region breaks
 * every later C++ use of NULL. */
#if defined(__cplusplus) || !__is_identifier(wchar_t)
#if (!defined(__MINGW32__) && !defined(_MSC_VER)) || defined(_WIN32_ITANIUM) || defined(__NTPOSIX__)
/* Windows Itanium and NTPOSIX preserve __null via sal.h wrapper. */
#define NULL __null
#else
/* MSVC: SDK's sal.h redefines __null as a SAL annotation. */
#define NULL 0
#endif
#else
#define NULL ((void*)0)
#endif

#endif
