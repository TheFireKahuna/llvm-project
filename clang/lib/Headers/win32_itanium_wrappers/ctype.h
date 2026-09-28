/*===---- ctype.h - UCRT ctype.h wrapper ------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CTYPE_H
#define __CLANG_CTYPE_H

/* The UCRT's POSIX and other non-standard names are declared unless the mode
 * is strict ISO C; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_NONSTDC_NAMES)
#ifndef __cplusplus
/* The UCRT defines _chvalidchk_l, _ischartype_l and the helpers they use, which
 * the _is*_l macros expand to, __inline without extern, and does not export
 * them. Outside the Microsoft C++ ABI such a C definition is only an inline
 * definition, so a call that is not inlined would refer to a function that no
 * library defines. Give them internal linkage instead. The __ascii_* helpers
 * are __forceinline, which under GNU89 inline semantics, before C99 or with
 * -fgnu89-inline, would be defined in every unit; extern makes them inline
 * definitions there too. */
#pragma push_macro("__inline")
#pragma push_macro("__forceinline")
#define __inline static __inline
#if defined(__GNUC_GNU_INLINE__) || !defined(__STDC_VERSION__) ||              \
    __STDC_VERSION__ < 199901L
#define __forceinline extern __forceinline
#endif
#include_next <ctype.h>
#pragma pop_macro("__forceinline")
#pragma pop_macro("__inline")
#else
#include_next <ctype.h>
#endif
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

#endif /* __CLANG_CTYPE_H */
