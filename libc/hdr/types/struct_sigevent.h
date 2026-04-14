//===-- Proxy for struct sigevent -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_HDR_TYPES_STRUCT_SIGEVENT_H
#define LLVM_LIBC_HDR_TYPES_STRUCT_SIGEVENT_H

#if defined(LIBC_FULL_BUILD) || defined(_WIN32) || defined(_WIN32_ITANIUM)

#include "include/llvm-libc-types/struct_sigevent.h"

#else // Overlay mode

#include <signal.h>

#endif // LIBC_FULL_BUILD

#endif // LLVM_LIBC_HDR_TYPES_STRUCT_SIGEVENT_H
