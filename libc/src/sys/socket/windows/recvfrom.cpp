//===-- Windows implementation of recvfrom --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// For connected SOCK_STREAM sockets, recvfrom delegates to recv() and
// optionally fills the source address from cached remote state.
//
//===----------------------------------------------------------------------===//

#include "src/sys/socket/recvfrom.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/recvfrom.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, recvfrom,
                   (int sockfd, void *buf, size_t len, int flags,
                    struct sockaddr *src_addr, socklen_t *addrlen)) {
  auto result =
      windows_syscalls::recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
