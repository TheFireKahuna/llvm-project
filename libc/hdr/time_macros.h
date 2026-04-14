//===-- Definition of macros from time.h ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_HDR_TIME_MACROS_H
#define LLVM_LIBC_HDR_TIME_MACROS_H

#ifdef LIBC_FULL_BUILD

#include "include/llvm-libc-macros/time-macros.h"

#else // Overlay mode

#include <time.h>

// Windows system <time.h> (UCRT) does not define POSIX clock constants
// (CLOCK_MONOTONIC, CLOCK_REALTIME, etc.) or timer macros. Pull in the
// extension header so that overlay-mode code on any Windows target can use
// the POSIX time interfaces provided by llvm-libc.
// In full-build mode the platform dispatcher in time-macros.h already
// includes this header for __NTPOSIX__ and other Windows environments.
#ifdef _WIN32
#include "include/llvm-libc-macros/windows/time-macros-ext.h"
#endif

#endif // LLVM_LIBC_FULL_BUILD

#endif // LLVM_LIBC_HDR_TIME_MACROS_H
