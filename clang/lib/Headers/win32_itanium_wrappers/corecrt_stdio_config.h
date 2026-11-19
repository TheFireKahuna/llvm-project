/*===---- corecrt_stdio_config.h - UCRT stdio configuration wrapper --------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CORECRT_STDIO_CONFIG_H
#define __CLANG_CORECRT_STDIO_CONFIG_H

/* %s and %ls in the wide printf and scanf functions take char and wchar_t
 * strings, as ISO C specifies, unless the project asks for the legacy
 * behavior. */
#if !defined(_CRT_STDIO_ISO_WIDE_SPECIFIERS) &&                                \
    !defined(_CRT_STDIO_LEGACY_WIDE_SPECIFIERS)
#define _CRT_STDIO_ISO_WIDE_SPECIFIERS
#endif

#include_next <corecrt_stdio_config.h>

#endif /* __CLANG_CORECRT_STDIO_CONFIG_H */
