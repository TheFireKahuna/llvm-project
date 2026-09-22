/*===---- winnt.h - Windows NT definitions for Itanium --------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_winnt_h
#define __clang_winnt_h

#if defined(_WIN32_ITANIUM)
    #ifndef TYPE_ALIGNMENT
    #define TYPE_ALIGNMENT(t) __alignof(t)
    #endif


    #ifndef DECLSPEC_NORETURN
    #if !defined(MIDL_PASS)
    #define DECLSPEC_NORETURN   __declspec(noreturn)
    #else
    #define DECLSPEC_NORETURN
    #endif
    #endif

    #ifndef DECLSPEC_NOTHROW
    #if !defined(MIDL_PASS)
    #define DECLSPEC_NOTHROW   __declspec(nothrow)
    #else
    #define DECLSPEC_NOTHROW
    #endif
    #endif

    #ifndef DECLSPEC_RESTRICT
    #if !defined(MIDL_PASS)
    #define DECLSPEC_RESTRICT   __declspec(restrict)
    #else
    #define DECLSPEC_RESTRICT
    #endif
    #endif

    #ifndef DECLSPEC_UUID
    #if defined(__cplusplus)
    #define DECLSPEC_UUID(x)    __declspec(uuid(x))
    #else
    #define DECLSPEC_UUID(x)
    #endif
    #endif

    #ifndef DECLSPEC_NOVTABLE
    #if defined(__cplusplus)
    #define DECLSPEC_NOVTABLE   __declspec(novtable)
    #else
    #define DECLSPEC_NOVTABLE
    #endif
    #endif

    #ifndef DECLSPEC_SELECTANY
    #define DECLSPEC_SELECTANY  __declspec(selectany)
    #endif



    #ifndef DECLSPEC_SAFEBUFFERS
    #define DECLSPEC_SAFEBUFFERS  __declspec(safebuffers)
    #endif

    #ifndef DECLSPEC_NOINLINE
    #define DECLSPEC_NOINLINE  __declspec(noinline)
    #endif




    #ifndef FORCEINLINE
    #define FORCEINLINE __forceinline
    #endif

    #ifndef DECLSPEC_DEPRECATED
    #if !defined(MIDL_PASS)
    #define DECLSPEC_DEPRECATED   __declspec(deprecated)
    #define DEPRECATE_SUPPORTED
    #else
    #define DECLSPEC_DEPRECATED
    #undef  DEPRECATE_SUPPORTED
    #endif
    #endif

    #ifdef __cplusplus
    #undef WIN_NOEXCEPT
    #if __cplusplus >= 201103L
    #define WIN_NOEXCEPT noexcept
    #else
    #define WIN_NOEXCEPT throw()
    #endif
    #else
    #define WIN_NOEXCEPT
    #endif

    #undef DEFAULT_UNREACHABLE
    #define DEFAULT_UNREACHABLE default: __builtin_unreachable()

    #if defined(__cplusplus) && __cplusplus >= 201103L
    #undef _ENUM_FLAG_CONSTEXPR
    #define _ENUM_FLAG_CONSTEXPR constexpr
    #else
    #define _ENUM_FLAG_CONSTEXPR
    #endif

    #undef DECLSPEC_ALIGN
    #define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#endif

#pragma push_macro("WIN_NOEXCEPT")
#pragma push_macro("TYPE_ALIGNMENT")
#pragma push_macro("FORCEINLINE")
#pragma push_macro("DEFAULT_UNREACHABLE")
#pragma push_macro("_ENUM_FLAG_CONSTEXPR")
#if __has_include_next(<winnt.h>) && !defined(_NTDEF_)
#include_next <winnt.h>
#endif
#pragma pop_macro("WIN_NOEXCEPT")
#pragma pop_macro("TYPE_ALIGNMENT")
#pragma pop_macro("FORCEINLINE")
#pragma pop_macro("DEFAULT_UNREACHABLE")
#pragma pop_macro("_ENUM_FLAG_CONSTEXPR")



#endif /* __clang_winnt_h */
