/*===---- sal.h - Windows SAL annotation wrapper ----------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_winnt_h
#define __clang_winnt_h

//
// For compilers that don't support nameless unions/structs
//
#if defined(_WIN32_ITANIUM)
    #ifndef DUMMYUNIONNAME
        #define DUMMYUNIONNAME
        #define DUMMYUNIONNAME2  
        #define DUMMYUNIONNAME3  
        #define DUMMYUNIONNAME4  
        #define DUMMYUNIONNAME5  
        #define DUMMYUNIONNAME6  
        #define DUMMYUNIONNAME7  
        #define DUMMYUNIONNAME8  
        #define DUMMYUNIONNAME9  
    #endif // DUMMYUNIONNAME

    #ifndef DUMMYSTRUCTNAME
        #define DUMMYSTRUCTNAME  
        #define DUMMYSTRUCTNAME2 
        #define DUMMYSTRUCTNAME3 
        #define DUMMYSTRUCTNAME4 
        #define DUMMYSTRUCTNAME5 
        #define DUMMYSTRUCTNAME6 
    #endif // DUMMYSTRUCTNAME

    #ifdef __cplusplus
    #define TYPE_ALIGNMENT( t ) __alignof(t)
    #endif

    #ifndef DECLSPEC_IMPORT
    #if (defined(_M_IX86) || defined(_M_IA64) || defined(_M_AMD64) || defined(_M_ARM) || defined(_M_ARM64)) && !defined(MIDL_PASS)
    #define DECLSPEC_IMPORT __declspec(dllimport)
    #else
    #define DECLSPEC_IMPORT
    #endif
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

    #ifndef NOP_FUNCTION
    #define NOP_FUNCTION (void)0
    #endif

    #ifndef DECLSPEC_ADDRSAFE
    #if (defined(_M_ALPHA) || defined(_M_AXP64))
    #define DECLSPEC_ADDRSAFE  __declspec(address_safe)
    #else
    #define DECLSPEC_ADDRSAFE
    #endif
    #endif

    #ifndef DECLSPEC_SAFEBUFFERS
    #define DECLSPEC_SAFEBUFFERS  __declspec(safebuffers)
    #endif

    #ifndef DECLSPEC_NOINLINE
    #define DECLSPEC_NOINLINE  __declspec(noinline)
    #endif

    #ifndef DECLSPEC_NOSANITIZEADDRESS
    #if defined(__SANITIZE_ADDRESS__)
    #define DECLSPEC_NOSANITIZEADDRESS      __declspec(no_sanitize_address)
    #define ASAN_WARNING_DISABLE_4714_PUSH  __pragma(warning(push)) __pragma(warning(disable:4714))
    #define ASAN_WARNING_DISABLE_4714_POP   __pragma(warning(pop))
    #else
    #define DECLSPEC_NOSANITIZEADDRESS
    #define ASAN_WARNING_DISABLE_4714_PUSH
    #define ASAN_WARNING_DISABLE_4714_POP
    #endif
    #endif

    #ifndef DECLSPEC_GUARDNOCF
    #define DECLSPEC_GUARDNOCF  __declspec(guard(nocf))
    #endif

    #ifndef DECLSPEC_GUARD_SUPPRESS
    #define DECLSPEC_GUARD_SUPPRESS  __declspec(guard(suppress))
    #endif

    #ifndef DECLSPEC_CHPE_GUEST
    #if _M_HYBRID
    #define DECLSPEC_CHPE_GUEST  __declspec(hybrid_guest)
    #else
    #define DECLSPEC_CHPE_GUEST
    #endif
    #endif

    #ifndef DECLSPEC_CHPE_PATCHABLE
    #if !defined(SORTPP_PASS)
    #if defined (_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    #define DECLSPEC_CHPE_PATCHABLE  __declspec(hybrid_patchable)
    #else
    #define DECLSPEC_CHPE_PATCHABLE  DECLSPEC_NOINLINE
    #endif
    #else
    #define DECLSPEC_CHPE_PATCHABLE
    #endif
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
#endif
/* Preserve Clang's __null builtin around the SDK include since Windows SDK
 * sal.h may redefine it. */
/*
 * When wchar_t is 32-bit (llvm-libc on Windows Itanium), the SDK's
 * `typedef wchar_t WCHAR` produces a 32-bit WCHAR — wrong for the Win32
 * ABI which requires 16-bit.  Redirect wchar_t → __CHAR16_TYPE__ for the
 * duration of the SDK include so every SDK type (WCHAR, LPCWSTR, struct
 * members, function parameters) is 16-bit.
 */
#if defined(_WIN32_ITANIUM) && __SIZEOF_WCHAR_T__ == 4
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#ifdef __cplusplus
#define wchar_t char16_t
#else
#define wchar_t unsigned short
#endif
#pragma clang diagnostic pop
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

#if defined(_WIN32_ITANIUM) && __SIZEOF_WCHAR_T__ == 4
#undef wchar_t
#endif

#ifdef _WIN32_ITANIUM
/* UCRT wide string functions the SDK depends on (stralign.h uses _wcsicmp).
   Normally provided by corecrt_wstring.h; declared here so the SDK compiles
   without UCRT headers. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int __cdecl _wcsicmp(const WCHAR *, const WCHAR *);
int __cdecl _wcsnicmp(const WCHAR *, const WCHAR *, size_t);
#ifdef __cplusplus
}
#endif
#endif /* _WIN32_ITANIUM */

#endif /* __clang_winnt_h */
