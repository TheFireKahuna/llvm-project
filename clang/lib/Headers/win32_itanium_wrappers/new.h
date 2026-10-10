/*===---- new.h - UCRT new.h wrapper ---------------------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_NEW_H
#define __CLANG_NEW_H

#ifdef __cplusplus
/* With _MSC_EXTENSIONS defined the UCRT's new.h declares std::new_handler
 * and std::set_new_handler itself, which the C++ library's <new> owns;
 * without it, new.h includes <new> instead. The headers new.h includes first
 * are included here, so that only new.h itself sees the macro hidden. */
#include <corecrt.h>
#include <new>
#include <vcruntime_new_debug.h>
#pragma push_macro("_MSC_EXTENSIONS")
#undef _MSC_EXTENSIONS
#include_next <new.h>
#pragma pop_macro("_MSC_EXTENSIONS")
#else
#include_next <new.h>
#endif

#endif /* __CLANG_NEW_H */
