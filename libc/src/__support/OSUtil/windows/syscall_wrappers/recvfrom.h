//===-- windows_syscalls::recvfrom() wrapper -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_RECVFROM_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_RECVFROM_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "src/__support/OSUtil/windows/ipc/socket_engine.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<ssize_t> recvfrom(int sockfd, void *buf, size_t len,
                                      int flags, struct sockaddr *src_addr,
                                      socklen_t *addrlen) {
  intptr_t ret = internal::recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return static_cast<ssize_t>(ret);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_RECVFROM_H
