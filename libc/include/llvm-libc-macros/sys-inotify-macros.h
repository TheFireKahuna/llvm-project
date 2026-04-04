//===-- Macros defined in sys/inotify.h header file ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_SYS_INOTIFY_MACROS_H
#define LLVM_LIBC_MACROS_SYS_INOTIFY_MACROS_H

#ifdef __linux__
#include "linux/sys-inotify-macros.h"
#elif defined(__NTPOSIX__)
#include "windows/sys-inotify-macros.h"
#endif

#endif // LLVM_LIBC_MACROS_SYS_INOTIFY_MACROS_H
