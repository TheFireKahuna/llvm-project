//===-- Windows implementation of futex ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to windows_syscalls::futex() wrapper.
//
//===----------------------------------------------------------------------===//

#include "src/sys/futex/futex.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/futex.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(long, futex,
                   (volatile uint32_t *uaddr, int futex_op, uint32_t val,
                    const struct timespec *timeout, volatile uint32_t *uaddr2,
                    uint32_t val3)) {
  auto result =
      windows_syscalls::futex(uaddr, futex_op, val, timeout, uaddr2, val3);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
