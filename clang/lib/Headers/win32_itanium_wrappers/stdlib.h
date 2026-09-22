/*===---- stdlib.h - Standard library wrapper ------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_STDLIB_H
#define __CLANG_STDLIB_H

#if __STDC_HOSTED__ && __has_include_next(<stdlib.h>)

#if defined(__cplusplus) && defined(_WIN32_ITANIUM)
/*
 * Present the UCRT as a plain C library: libc++ owns every C++-visible
 * declaration. UCRT's __cplusplus blocks declare inline abs/div overloads
 * that collide with libc++'s definitions, so include the header with
 * __cplusplus hidden. corecrt.h is included first, while __cplusplus is
 * still visible, so _CRT_BEGIN_C_HEADER is permanently pinned to its
 * extern "C" form and every UCRT declaration keeps C linkage.
 *
 * While __cplusplus is hidden the TU must still identify as some language:
 * headers reached from here (e.g. <limits.h> from UCRT stdlib.h) run
 * version-gated content once, under their include guard, and would
 * otherwise lock in the no-dialect subset for the whole TU. Claim C17 so
 * they take their modern-C paths, whose results match the C++ ones.
 */
/* Hiding __cplusplus can make UCRT define min/max. Restore the caller's
 * macro state after including it, without exposing those C-only macros. */
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include <corecrt.h>
/* UCRT assumes every C++ dialect supports noexcept. Keep its non-throwing
 * declarations valid when libc++ uses the frozen C++03 headers. */
#if __cplusplus < 201103L
#undef _CRT_NOEXCEPT
#define _CRT_NOEXCEPT throw()
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__cplusplus")
#undef __cplusplus
#pragma push_macro("__STDC_VERSION__")
#define __STDC_VERSION__ 201710L
#include_next <stdlib.h>
#pragma pop_macro("__STDC_VERSION__")
#pragma pop_macro("__cplusplus")
#pragma clang diagnostic pop
#pragma pop_macro("max")
#pragma pop_macro("min")
#else
#include_next <stdlib.h>
#endif

/*
 * aligned_alloc: C11 7.22.3.1. The UCRT neither declares nor exports it, and
 * wincrt supplies allocations compatible with ordinary UCRT free/realloc.
 * Fundamental alignments use native allocation; extended ones require a
 * qualified native segment heap (see compiler-rt/lib/wincrt/README.md).
 * Declared here so libc++'s <cstdlib> using_if_exists resolves
 * std::aligned_alloc and C code sees the C11 name. Unsupported alignments
 * return a null pointer.
 */
#if defined(_WIN32_ITANIUM)
#ifdef __cplusplus
extern "C" {
#endif
void *__cdecl aligned_alloc(size_t alignment, size_t size);
/* POSIX aligned allocation: returns an error number without changing errno.
 * Alignment must be a power of two and a multiple of sizeof(void *).
 * On failure *memptr is unchanged; on zero-size success it is null. */
int __cdecl posix_memalign(void **memptr, size_t alignment, size_t size);
#ifdef __cplusplus
}
#endif
#endif

#endif

/*
 * Windows Itanium: Map POSIX function names to UCRT underscore-prefixed names.
 *
 * The UCRT only exposes putenv/itoa/swab etc. when !__STDC__, but we want
 * __STDC__ for standards compliance. Provide the mappings when targeting
 * MSVCRT/UCRT.
 */
#if defined(__MSVCRT__) || defined(_UCRT)
/* Environment */
#  ifndef putenv
#    define putenv _putenv
#  endif
/* Number/string conversion */
#  ifndef ecvt
#    define ecvt _ecvt
#  endif
#  ifndef fcvt
#    define fcvt _fcvt
#  endif
#  ifndef gcvt
#    define gcvt _gcvt
#  endif
#  ifndef itoa
#    define itoa _itoa
#  endif
#  ifndef ltoa
#    define ltoa _ltoa
#  endif
#  ifndef ultoa
#    define ultoa _ultoa
#  endif
/* Byte swapping */
#  ifndef swab
#    define swab _swab
#  endif
#endif /* __MSVCRT__ || _UCRT */

#endif /* __CLANG_STDLIB_H */
