//===-- NT-POSIX extern-C getpid() shim -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Signal tests call extern-C getpid() to cross-check siginfo_t::si_pid
// against the sender's PID. __internal__ packaging doesn't emit the
// extern-C alias; opt-in via libc.src.unistd.getpid in the test DEPS.
//
// Per-shim TU so a test that doesn't use getpid() isn't forced to resolve
// LIBC_NAMESPACE::getpid (see HermeticTestUtilsNTPOSIX.cpp banner).
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
int getpid();
} // namespace LIBC_NAMESPACE_DECL

extern "C" int getpid() { return LIBC_NAMESPACE::getpid(); }
