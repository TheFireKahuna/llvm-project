//===-- windows_syscalls::pselect() wrapper -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_PSELECT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_PSELECT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "src/__support/OSUtil/windows/ipc/select_ops.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<int> pselect(int nfds, fd_set *__restrict read_set,
                                 fd_set *__restrict write_set,
                                 fd_set *__restrict error_set,
                                 const struct timespec *__restrict timeout,
                                 const sigset_t *__restrict sigmask) {
  intptr_t ret =
      internal::pselect(nfds, read_set, write_set, error_set, timeout, sigmask);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return static_cast<int>(ret);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_PSELECT_H
