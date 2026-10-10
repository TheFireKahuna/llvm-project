/*===---- io.h - UCRT io.h wrapper -----------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_IO_H
#define __CLANG_IO_H

/* Not an ISO C header, so the UCRT's POSIX and other non-standard names are
 * declared in every mode; see corecrt.h. */
#include <corecrt.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma push_macro("__STDC__")
#undef __STDC__
#define __STDC__ (!__CLANG_UCRT_POSIX_HEADER_NAMES)
#include_next <io.h>
#pragma pop_macro("__STDC__")
#pragma clang diagnostic pop

#endif /* __CLANG_IO_H */
