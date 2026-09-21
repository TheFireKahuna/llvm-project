/*===---- sal.h - Windows SAL annotation wrapper ----------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_sal_h
#define __clang_sal_h

/* Preserve Clang's __null builtin around the SDK include since Windows SDK
 * sal.h may redefine it. */
#pragma push_macro("__null")
#if __has_include_next(<sal.h>)
#include_next <sal.h>
#endif
#pragma pop_macro("__null")

#endif /* __clang_sal_h */
