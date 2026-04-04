//===--------------- Internal syscall declarations --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_SYSCALL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_SYSCALL_H

#include "src/__support/macros/properties/runtime.h"

#ifdef __APPLE__
#include "darwin/syscall.h"
#include <sys/syscall.h>
#elif defined(__linux__)
#include "linux/syscall.h"
#include <sys/syscall.h>
#elif defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
// Windows: SYS_* constants and syscall_impl dispatch are both provided
// by the Windows header — no separate <sys/syscall.h> needed.
#include "windows/syscall.h"
#endif

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_SYSCALL_H
