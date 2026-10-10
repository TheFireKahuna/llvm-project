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

/* Not an ISO C header, so the UCRT's POSIX and other non-standard names are
 * declared in every mode; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_POSIX_HEADER_NAMES)
#include_next <malloc.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

/* The UCRT defines these as macros for _aligned_malloc and _aligned_free.
 * Clang's mm_malloc.h defines them with posix_memalign and free instead, and
 * malloc.h provides them, as the UCRT's does. */
#undef _mm_malloc
#undef _mm_free
#include <mm_malloc.h>

#endif /* __CLANG_MALLOC_H */
