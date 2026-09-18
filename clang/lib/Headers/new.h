/*===---- new.h - UCRT new.h wrapper for Windows Itanium ------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 *
 * UCRT new.h includes <crtdefs.h> when _MSC_EXTENSIONS is defined. On Windows
 * Itanium / libc++ builds, there is no MSVC runtime and <crtdefs.h> is not
 * available. This wrapper forces the !_MSC_EXTENSIONS path for those builds
 * while still including the real UCRT new.h to get its C declarations.
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_UCRT_NEW_H
#define __CLANG_UCRT_NEW_H

#include <corecrt.h>
#include <vcruntime_new_debug.h>


#if defined(_MSC_VER) && !defined(_WIN32_ITANIUM)
/* Non-Itanium MSVC/clang-cl: pass through to the real header. */
#if __has_include_next(<new.h>)
#include_next <new.h>
#endif
#else

#ifndef _INC_NEW // include guard for 3rd party interop
#define _INC_NEW
#ifdef __cplusplus
    #include <new>
#endif

_CRT_BEGIN_C_HEADER

typedef int (__CRTDECL* _PNH)(size_t);

_PNH __cdecl _query_new_handler(void);
_PNH __cdecl _set_new_handler(_In_opt_ _PNH _NewHandler);

// new mode flag -- when set, makes malloc() behave like new()
_ACRTIMP int __cdecl _query_new_mode(void);
_ACRTIMP int __cdecl _set_new_mode(_In_ int _NewMode);


_CRT_END_C_HEADER
#endif
#endif
#endif /* __CLANG_UCRT_NEW_H */
