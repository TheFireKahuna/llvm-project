//===-- windows_syscalls::getrandom() wrapper ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETRANDOM_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETRANDOM_H

#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<ssize_t> getrandom(void *buf, size_t buflen,
                                        unsigned int flags) {
  // Only GRND_RANDOM, GRND_NONBLOCK, GRND_INSECURE are valid.
  constexpr unsigned int VALID_FLAGS = 0x0007;
  if (flags & ~VALID_FLAGS)
    return Error(EINVAL);

  if (buf == nullptr && buflen > 0)
    return Error(EFAULT);

  if (buflen == 0)
    return ssize_t(0);

  // ProcessPrng always succeeds and uses the kernel CSPRNG directly.
  ::ProcessPrng(static_cast<unsigned char *>(buf), buflen);
  return static_cast<ssize_t>(buflen);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETRANDOM_H
