/*===---- vadefs.h - Variable argument definitions the UCRT expects --------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* Windows Itanium has no Visual C++ headers. This provides what the Windows
 * SDK and the UCRT use from the Visual C++ runtime's vadefs.h, which clang's
 * own vadefs.h includes outside MSVC. */

#ifndef __CLANG_VADEFS_H
#define __CLANG_VADEFS_H

#define _CRT_PACKING 8

#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef __UINTPTR_TYPE__ uintptr_t;
#endif

#ifndef _VA_LIST
#define _VA_LIST
typedef __builtin_va_list va_list;
#endif

#define __crt_va_start(ap, param) __builtin_va_start(ap, param)
#define __crt_va_end(ap) __builtin_va_end(ap)
#define __crt_va_arg(ap, type) __builtin_va_arg(ap, type)

#endif /* __CLANG_VADEFS_H */
