//===-- Windows implementation of sendto ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/socket/sendto.h"

#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "src/__support/OSUtil/windows/ipc/socket_engine.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, sendto,
                   (int sockfd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen)) {
  ssize_t r = internal::sendto(sockfd, buf, len, flags, dest_addr, addrlen);
  if (r < 0) {
    libc_errno = static_cast<int>(-r);
    return -1;
  }
  return r;
}

} // namespace LIBC_NAMESPACE_DECL
