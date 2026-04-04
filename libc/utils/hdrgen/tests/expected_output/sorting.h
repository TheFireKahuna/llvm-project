//===-- Standard C header <sorting.h> --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#ifndef _LLVM_LIBC_SORTING_H
#define _LLVM_LIBC_SORTING_H

#if !defined(_WIN32) || defined(__NTPOSIX__)

#include "__llvm-libc-common.h"

__BEGIN_C_DECLS

__LIBC_FUNC_IMPORT void func_with_aliases(int) __NOEXCEPT;
__LIBC_FUNC_IMPORT void _func_with_aliases(int) __NOEXCEPT;
__LIBC_FUNC_IMPORT void __func_with_aliases(int) __NOEXCEPT;

__LIBC_FUNC_IMPORT void gunk(const char *) __NOEXCEPT;

__END_C_DECLS

#endif // !defined(_WIN32) || defined(__NTPOSIX__)
#endif // _LLVM_LIBC_SORTING_H

#if defined(_WIN32) && !defined(__NTPOSIX__) && __has_include_next(<sorting.h>)
#include_next <sorting.h>
#endif
