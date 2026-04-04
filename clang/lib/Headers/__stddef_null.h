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

#ifdef __cplusplus
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
