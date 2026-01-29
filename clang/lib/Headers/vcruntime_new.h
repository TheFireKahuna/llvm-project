/* ===-------- vcruntime_new.h --------------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_vcruntime_new_h
#define __clang_vcruntime_new_h

/*
 * When using libc++ with Itanium ABI on Windows (Windows Itanium), libc++
 * provides its own definitions of nothrow_t, align_val_t, and placement
 * new/delete. We must prevent vcruntime_new.h from redefining these.
 *
 * These guard macros are checked by MSVC's vcruntime_new.h before defining
 * its types. By defining them first, the real header skips its definitions.
 *
 * For align_val_t, vcruntime uses #ifdef __cpp_aligned_new with no guard
 * macro, so we must undefine it. libc++ uses _LIBCPP_HAS_LIBRARY_ALIGNED_ALLOCATION
 * instead, so this doesn't affect libc++'s definition.
 */
#if defined(_LIBCPP_ABI_FORCE_ITANIUM) && defined(_MSC_VER)
#  define __NOTHROW_T_DEFINED
#  define __PLACEMENT_NEW_INLINE
#  define __PLACEMENT_VEC_NEW_INLINE
/* Skip MSVC debug allocator declarations in vcruntime_new_debug.h.
   We don't link vcruntime so these operators aren't available. */
#  define _MFC_OVERRIDES_NEW
#  ifdef __cpp_aligned_new
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#    undef __cpp_aligned_new
#    pragma clang diagnostic pop
#  endif
#endif

#include_next <vcruntime_new.h>

#endif /* __clang_vcruntime_new_h */
