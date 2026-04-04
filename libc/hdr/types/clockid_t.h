//===-- Proxy for clockid_t -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_HDR_TYPES_CLOCKID_T_H
#define LLVM_LIBC_HDR_TYPES_CLOCKID_T_H

// clockid_t is not provided by any Windows system header (UCRT or otherwise).
// Always use the llvm-libc definition on Windows targets (_WIN32 is defined
// for all Windows environments including NTPOSIX and Windows Itanium).
#if defined(LIBC_FULL_BUILD) || defined(_WIN32)

#include "include/llvm-libc-types/clockid_t.h"

#else // Overlay mode

#include <sys/types.h>

#endif // LLVM_LIBC_FULL_BUILD

#endif // LLVM_LIBC_HDR_TYPES_CLOCKID_T_H
