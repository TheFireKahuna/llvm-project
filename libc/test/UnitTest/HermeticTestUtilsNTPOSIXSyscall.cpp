//===-- NT-POSIX extern-C syscall() shim ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// NT-POSIX's public syscall() is variadic; under freestanding-unit link the
// __internal__ entrypoint objects only carry the C++-mangled form, so the
// extern-C alias <unistd.h> declares is absent. Unpack the va_list and
// route to the fixed-7-arg entrypoint. Pointer args flow through intptr_t
// to survive LLP64's 32-bit long.
//
// Per-shim TU (see HermeticTestUtilsNTPOSIX.cpp banner) so extracting this
// object into a test only costs the test a DEP on the fixed-arg entrypoint.
//
//===----------------------------------------------------------------------===//

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

long __llvm_libc_syscall(long number, long arg1, long arg2, long arg3,
                         long arg4, long arg5, long arg6);

} // namespace LIBC_NAMESPACE_DECL

extern "C" long syscall(long number, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, number);
  intptr_t a1 = __builtin_va_arg(ap, intptr_t);
  intptr_t a2 = __builtin_va_arg(ap, intptr_t);
  intptr_t a3 = __builtin_va_arg(ap, intptr_t);
  intptr_t a4 = __builtin_va_arg(ap, intptr_t);
  intptr_t a5 = __builtin_va_arg(ap, intptr_t);
  intptr_t a6 = __builtin_va_arg(ap, intptr_t);
  __builtin_va_end(ap);
  return LIBC_NAMESPACE::__llvm_libc_syscall(
      number, static_cast<long>(a1), static_cast<long>(a2),
      static_cast<long>(a3), static_cast<long>(a4), static_cast<long>(a5),
      static_cast<long>(a6));
}
