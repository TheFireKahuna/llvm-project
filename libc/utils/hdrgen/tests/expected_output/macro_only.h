//===-- Standard C header <macro_only.h> --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#ifndef _LLVM_LIBC_MACRO_ONLY_H
#define _LLVM_LIBC_MACRO_ONLY_H

#if !defined(_WIN32) || defined(__NTPOSIX__)

#include "__llvm-libc-common.h"

#define MACRO_A 1

#endif // !defined(_WIN32) || defined(__NTPOSIX__)
#endif // _LLVM_LIBC_MACRO_ONLY_H

#if defined(_WIN32) && !defined(__NTPOSIX__) && __has_include_next(<macro_only.h>)
#include_next <macro_only.h>
#endif
