/*===---- vcruntime_new.h - MSVC new operator wrapper ----------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_VCRUNTIME_NEW_H
#define __CLANG_VCRUNTIME_NEW_H

/*
 * When using libc++ (-stdlib=libc++), the <new> header already defines
 * align_val_t, nothrow_t, and placement new/delete. Including MSVC's
 * vcruntime_new.h would cause redefinition errors.
 *
 * This wrapper skips vcruntime_new.h entirely when libc++ is in use,
 * since libc++ provides all the required definitions.
 *
 * Detection methods:
 * - _LIBCPP_NEW: libc++ <new> already included
 * - _LIBCPP_VERSION: any libc++ header already included
 * - _LIBCPP_NO_VCRUNTIME: explicit opt-out from vcruntime
 * - _WIN32_ITANIUM: Windows Itanium always uses libc++
 */
#if defined(_LIBCPP_NEW) || defined(_LIBCPP_VERSION) || \
    defined(_LIBCPP_NO_VCRUNTIME) || defined(_WIN32_ITANIUM)
/* libc++ in use or vcruntime explicitly disabled - skip vcruntime_new.h */
#elif __has_include_next(<vcruntime_new.h>)
#include_next <vcruntime_new.h>
#endif

#endif /* __CLANG_VCRUNTIME_NEW_H */
