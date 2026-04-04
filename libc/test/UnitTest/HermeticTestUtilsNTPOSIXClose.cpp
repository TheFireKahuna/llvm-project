//===-- NT-POSIX extern-C close() shim ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests written against the POSIX C API (e.g. assert_test's EXPECT_DEATH
// body) forward-declare extern-C close() and call it directly. The
// __internal__ packaging doesn't emit that alias, so we bridge it here.
// Test must opt in by DEPENDing on libc.src.unistd.close so the
// LIBC_NAMESPACE::close entrypoint obj is pulled into the link.
//
// Per-shim TU so a test that doesn't use close() doesn't get forced to
// resolve LIBC_NAMESPACE::close (see HermeticTestUtilsNTPOSIX.cpp banner).
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
int close(int fd);
} // namespace LIBC_NAMESPACE_DECL

extern "C" int close(int fd) { return LIBC_NAMESPACE::close(fd); }
