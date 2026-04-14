//===-- Lightweight futex include dispatcher ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Use this header when you need Futex / FutexWordType without pulling in the
// full RawMutex machinery (sleep, timeouts, monotonicity, etc.).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_FUTEX_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_FUTEX_H

#include "src/__support/macros/properties/runtime.h"

#if defined(__linux__)
#include "src/__support/threads/linux/futex_utils.h"
#elif defined(__APPLE__)
#include "src/__support/threads/darwin/futex_utils.h"
#elif defined(LIBC_TARGET_RUNTIME_IS_NTPOSIX)
#include "src/__support/threads/windows/futex_utils.h"
#endif

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_FUTEX_H
