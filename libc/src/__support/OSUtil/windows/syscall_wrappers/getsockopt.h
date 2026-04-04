//===-- windows_syscalls::getsockopt() wrapper -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETSOCKOPT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETSOCKOPT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "hdr/types/socklen_t.h"

#include "src/__support/OSUtil/windows/ipc/socket_engine.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<int> getsockopt(int sockfd, int level, int optname,
                                    void *optval, socklen_t *optlen) {
  intptr_t ret = internal::getsockopt(sockfd, level, optname, optval, optlen);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return static_cast<int>(ret);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETSOCKOPT_H
