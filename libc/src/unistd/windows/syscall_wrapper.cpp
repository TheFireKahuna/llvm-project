//===-- Windows implementation of syscall() --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/syscall_wrapper.h"

#include "src/__support/OSUtil/syscall.h" // syscall_impl
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include <stdarg.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(long, syscall, (long number, ...)) {
  // On Windows LLP64, long is 32-bit — va_arg(ap, long) would truncate
  // 64-bit pointer arguments.  Use intptr_t (pointer-width) so pointers
  // survive the variadic round-trip.  This matches the Linux LP64 ABI
  // where long is already pointer-width.
  va_list ap;
  va_start(ap, number);
  intptr_t arg1 = va_arg(ap, intptr_t);
  intptr_t arg2 = va_arg(ap, intptr_t);
  intptr_t arg3 = va_arg(ap, intptr_t);
  intptr_t arg4 = va_arg(ap, intptr_t);
  intptr_t arg5 = va_arg(ap, intptr_t);
  intptr_t arg6 = va_arg(ap, intptr_t);
  va_end(ap);

  long ret = LIBC_NAMESPACE::syscall_impl<long>(
      static_cast<intptr_t>(number), arg1, arg2, arg3, arg4, arg5, arg6);
  // Use unsigned intptr_t for the sentinel check — on LLP64, unsigned long
  // is only 32 bits and would misclassify large positive returns as errors.
  if (static_cast<uintptr_t>(ret) > static_cast<uintptr_t>(-4096L)) {
    libc_errno = static_cast<int>(-ret);
    return -1;
  }
  return ret;
}

} // namespace LIBC_NAMESPACE_DECL
