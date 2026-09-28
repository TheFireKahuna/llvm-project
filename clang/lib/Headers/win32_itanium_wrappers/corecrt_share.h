/*===---- corecrt_share.h - UCRT corecrt_share.h wrapper -------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_CORECRT_SHARE_H
#define __CLANG_CORECRT_SHARE_H

/* The sharing modes of io.h and share.h. wchar.h includes this header too, and
 * the UCRT header is read once, so its names are declared in every mode; see
 * corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_POSIX_HEADER_NAMES)
#include_next <corecrt_share.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

#endif /* __CLANG_CORECRT_SHARE_H */
