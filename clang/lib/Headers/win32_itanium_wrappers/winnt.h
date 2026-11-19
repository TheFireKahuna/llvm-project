/*===---- winnt.h - Windows SDK winnt.h wrapper ----------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_WINNT_H
#define __CLANG_WINNT_H

#include <__winnt_declspec.h>
#include_next <winnt.h>

/* winnt.h defines these unconditionally, for a compiler older than Visual C++
 * 2015 when _MSC_VER is not defined. */
#undef DEFAULT_UNREACHABLE
#define DEFAULT_UNREACHABLE                                                    \
  default:                                                                     \
    __builtin_unreachable()

#if defined(__cplusplus) && __cplusplus >= 201103L
#undef WIN_NOEXCEPT
#define WIN_NOEXCEPT noexcept
#undef _ENUM_FLAG_CONSTEXPR
#define _ENUM_FLAG_CONSTEXPR constexpr
#endif

#endif /* __CLANG_WINNT_H */
