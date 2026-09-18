/*===---- vcruntime.h - MSVC new operator wrapper --------------------------===
*
* Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
* See https://llvm.org/LICENSE.txt for license information.
* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*
*===-----------------------------------------------------------------------===
*/

#ifndef __CLANG_VCRUNTIME_H
#define __CLANG_VCRUNTIME_H

/*
* This wrapper skips vcruntime.h entirely when libc++ is in use,
* resolving include errors from CoreCRT.h and other headers.
*
* Detection methods:
* - _LIBCPP_NO_VCRUNTIME: explicit opt-out from vcruntime
* - _WIN32_ITANIUM: Windows Itanium always uses libc++
*/
#if defined(_LIBCPP_NEW) || defined(_LIBCPP_VERSION) || \
defined(_LIBCPP_NO_VCRUNTIME) || defined(_WIN32_ITANIUM)

#define _VCRUNTIME_H
#ifndef _VCRT_COMPILER_PREPROCESSOR
    #define _VCRT_COMPILER_PREPROCESSOR 1
#endif
#ifndef _UCRT
    #define _UCRT
#endif

/* CRT import for backward compat */
#ifndef _CRTIMP
    #if defined CRTDLL && defined _CRTBLD
        #define _CRTIMP __declspec(dllexport)
    #else
        #ifdef _DLL
            #define _CRTIMP __declspec(dllimport)
        #else
            #define _CRTIMP
        #endif
    #endif
#define _VCRT_DEFINED_CRTIMP
#endif

#if !defined(_BEGIN_PRAGMA_OPTIMIZE_DISABLE)
    #define _BEGIN_PRAGMA_OPTIMIZE_DISABLE(flags, bug, reason) \
        _Pragma("clang optimize off")
    #define _BEGIN_PRAGMA_OPTIMIZE_ENABLE(flags, bug, reason) \
        _Pragma("clang optimize on")
    #define _END_PRAGMA_OPTIMIZE() \
        _Pragma("clang optimize on")
#endif

#include <sal.h>
#include <vadefs.h>
#include <stddef.h>

/* Define __int64 as a macro so it works with 'unsigned __int64' etc.
 * in Windows SDK headers. This is needed for targets where __int64
 * is not a built-in type keyword. */
#ifndef __int64
    #define __int64 __INT64_TYPE__
#endif
#ifndef __int32
    #define __int32 __INT32_TYPE__
#endif

#if defined __cplusplus
    #define _CRT_BEGIN_C_HEADER            \
        _Pragma("pack(push, 8)")           \
        extern "C" {

    #define _CRT_END_C_HEADER \
        }                     \
        _Pragma("pack(pop)")

#else

    #define _CRT_BEGIN_C_HEADER \
        _Pragma("pack(push, 8)")

    #define _CRT_END_C_HEADER \
        _Pragma("pack(pop)")

#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _HAS_EXCEPTIONS // Predefine as 0 to disable exceptions
    #ifdef _KERNEL_MODE
        #define _HAS_EXCEPTIONS 0
    #else
        #define _HAS_EXCEPTIONS 1
    #endif /* _KERNEL_MODE */
#endif /* _HAS_EXCEPTIONS */



#define _CRT_STRINGIZE_(x) #x
#define _CRT_STRINGIZE(x) _CRT_STRINGIZE_(x)

#define _CRT_WIDE_(s) L ## s
#define _CRT_WIDE(s) _CRT_WIDE_(s)

#define _CRT_CONCATENATE_(a, b) a ## b
#define _CRT_CONCATENATE(a, b)  _CRT_CONCATENATE_(a, b)

#define _CRT_UNPARENTHESIZE_(...) __VA_ARGS__
#define _CRT_UNPARENTHESIZE(...)  _CRT_UNPARENTHESIZE_ __VA_ARGS__

/* DLL import/export for vcruntime functions */
#ifndef _VCRTIMP
    #if defined _CRTIMP && !defined _VCRT_DEFINED_CRTIMP
        #define _VCRTIMP _CRTIMP
    #elif defined(_VCRT_BUILD) && defined(CRTDLL) && !defined(_VCRT_SAT_1)
        #define _VCRTIMP __declspec(dllexport)
    #else
        #define _VCRTIMP
    #endif
#endif

#ifndef _MRTIMP
    #if defined(MRTDLL) && defined(_CRTBLD) && !defined(_M_CEE_PURE)
        #define _MRTIMP __declspec(dllexport)
    #else
        #define _MRTIMP
    #endif
#endif

#if defined(_M_CEE_PURE) || defined(MRTDLL)
    #ifndef __CLRCALL_OR_CDECL
        #define __CLRCALL_OR_CDECL __clrcall
    #ifndef __CLR_OR_THIS_CALL
    #endif
        #define __CLR_OR_THIS_CALL __clrcall
    #endif
#else
    #define __CLRCALL_OR_CDECL __cdecl
    #define __CLR_OR_THIS_CALL
#endif

#ifdef _M_CEE_PURE
    #define __CLRCALL_PURE_OR_CDECL __clrcall
#else
    #define __CLRCALL_PURE_OR_CDECL __cdecl
#endif

#ifndef __CRTDECL
#define __CRTDECL __CLRCALL_PURE_OR_CDECL
#endif

#ifndef _CONST_RETURN
    #ifdef __cplusplus
        #define _CRT_CONST_CORRECT_OVERLOADS
        #define _CONST_RETURN  const
    #else
      #define _CONST_RETURN
    #endif
#endif

// For backwards compatibility
#ifndef _WConst_return
    #define _WConst_return const
#endif

// Definitions of common __declspecs
#define _VCRT_NOALIAS __attribute__((noalias))
#define _VCRT_RESTRICT __declspec(restrict)
#define _VCRT_ALLOCATOR __attribute__((malloc))
#if defined _M_CEE && defined _M_X64
    #define _VCRT_JIT_INTRINSIC __declspec(jitintrinsic)
#else
    #define _VCRT_JIT_INTRINSIC
#endif
#ifdef __midl
    #define _VCRT_ALIGN(x)
#else
    #define _VCRT_ALIGN(x) __attribute__((aligned(x)))
#endif

// Definitions of common types
#ifdef _WIN64
    #ifndef _SIZE_T_DEFINED
        #define _SIZE_T_DEFINED
        typedef long long unsigned int   size_t;
    #endif
    #ifndef _PTRDIFF_T_DEFINED
        #define _PTRDIFF_T_DEFINED
        typedef long long int            ptrdiff_t;
    #endif
    #ifndef _INTPTR_T_DEFINED
        #define _INTPTR_T_DEFINED
        typedef long long int            intptr_t;
    #endif
#else
    #ifndef _SIZE_T_DEFINED
        #define _SIZE_T_DEFINED
        typedef unsigned int             size_t;
    #endif
    #ifndef _PTRDIFF_T_DEFINED
        #define _PTRDIFF_T_DEFINED
        typedef int                      ptrdiff_t;
    #endif
    #ifndef _INTPTR_T_DEFINED
        #define _INTPTR_T_DEFINED
        typedef int                      intptr_t;
    #endif
#endif

#if defined __cplusplus
    typedef bool  __vcrt_bool;
#elif defined __midl
    // MIDL understands neither bool nor _Bool.  Use char as a best-fit
    // replacement (the differences won't matter in practice).
    typedef char __vcrt_bool;
#else
    typedef _Bool __vcrt_bool;
#endif

/* Define __unaligned as empty for targets where it's not a keyword.
 * x86/x64 handles unaligned access in hardware. */
#ifndef __unaligned
    #define __unaligned
#endif

#ifndef _UNALIGNED
    #define _UNALIGNED
#endif

#if defined(_M_ARM64EC) &&  !defined(__security_check_cookie)
    #define __security_check_cookie __security_check_cookie_arm64ec
#endif

#if defined __cplusplus
    #ifndef _STL_LANG
        #define _STL_LANG __cplusplus
    #endif
#else  // ^^^ determine compiler's C++ mode / no C++ support vvv
    #define _STL_LANG 0L
#endif

#define _MSVC_CONSTEXPR

#define __forceinline inline __attribute__((always_inline))

// [[nodiscard]] attributes on STL functions
#ifndef _HAS_NODISCARD
    #ifndef __cplusplus
        #define _HAS_NODISCARD 0
    #elif __has_cpp_attribute(nodiscard) >= 201603L // TRANSITION, VSO#939899 (need toolset update)
        #define _HAS_NODISCARD 1
    #else
        #define _HAS_NODISCARD 0
    #endif
#endif // _HAS_NODISCARD

#if _HAS_NODISCARD
    #define _NODISCARD [[nodiscard]]
#else 
    #define _NODISCARD
#endif // _HAS_NODISCARD

#ifndef _CRT_DEPRECATE_TEXT
    #define _CRT_DEPRECATE_TEXT(_Text) __declspec(deprecated(_Text))
#endif

#ifndef _CRT_INSECURE_DEPRECATE
    #define _CRT_INSECURE_DEPRECATE(_Replacement)
#endif

#ifndef _CRT_INSECURE_DEPRECATE_MEMORY
    #define _CRT_INSECURE_DEPRECATE_MEMORY _CRT_INSECURE_DEPRECATE
#endif

#ifdef __cplusplus
}
#endif
#endif

/* libc++ in use or vcruntime explicitly disabled - skip vcruntime.h */
#if __has_include_next(<vcruntime.h>)
#include_next <vcruntime.h>
#endif

// Definitions of common __declspecs
#undef _VCRT_NOALIAS
#undef _VCRT_RESTRICT
#undef _VCRT_ALLOCATOR
#undef _VCRT_JIT_INTRINSIC
#undef _VCRT_ALIGN
#define _VCRT_NOALIAS __attribute__((noalias))
#define _VCRT_RESTRICT __declspec(restrict)
#define _VCRT_ALLOCATOR __attribute__((malloc))
#if defined _M_CEE && defined _M_X64
    #define _VCRT_JIT_INTRINSIC __declspec(jitintrinsic)
#else
    #define _VCRT_JIT_INTRINSIC
#endif
#ifdef __midl
    #define _VCRT_ALIGN(x)
#else
    #define _VCRT_ALIGN(x) __attribute__((aligned(x)))
#endif

extern uintptr_t __security_cookie;

#ifndef _VCRT_BUILD
    #define __vcrt_malloc_normal(_Size) malloc(_Size)
    #define __vcrt_calloc_normal(_Count, _Size) calloc(_Count, _Size)
    #define __vcrt_free_normal(_Memory) free(_Memory)
#endif


#endif /* __CLANG_VCRUNTIME_H */
