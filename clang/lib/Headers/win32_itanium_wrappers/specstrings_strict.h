/*===---- specstrings_strict.h - Windows SAL strict wrapper ----------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* Only include this if we are aiming for MSVC compatibility. */
#if !defined(_WIN32_ITANIUM)
#include_next <specstrings_strict.h>
#else

#ifndef __clang_specstrings_strict_h
#define __clang_specstrings_strict_h

/* Preserve Clang's __null builtin before SDK redefines it. */
#pragma push_macro("__null")

#include_next <specstrings_strict.h>

/* Restore __null for NULL definition. */
#pragma pop_macro("__null")

#endif /* __clang_specstrings_strict_h */
#endif /* _MSC_VER || _WIN32_ITANIUM */
