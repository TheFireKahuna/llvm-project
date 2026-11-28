/*===---- malloc.h - UCRT malloc.h wrapper ---------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_MALLOC_H
#define __CLANG_MALLOC_H

/* Not a header that ISO C defines, so it declares the UCRT's POSIX and other
 * non-standard names in every mode, also where a header that ISO C defines is
 * the first to include it; see corecrt.h. */
#include <corecrt.h>
#pragma push_macro("__CLANG_UCRT_NONSTDC_NAMES")
#undef __CLANG_UCRT_NONSTDC_NAMES
#define __CLANG_UCRT_NONSTDC_NAMES 1
#include_next <malloc.h>
#pragma pop_macro("__CLANG_UCRT_NONSTDC_NAMES")

/* The UCRT defines these as macros for _aligned_malloc and _aligned_free,
 * whose memory free does not release. Clang's mm_malloc.h defines them over
 * posix_memalign and free instead, and malloc.h provides them, as the UCRT's
 * does. */
#undef _mm_malloc
#undef _mm_free
#include <mm_malloc.h>

#endif /* __CLANG_MALLOC_H */
