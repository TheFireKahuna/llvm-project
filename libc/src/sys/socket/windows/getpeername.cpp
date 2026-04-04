//===-- Windows implementation of getpeername ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::getpeername() in socket_engine.h.
//
//===----------------------------------------------------------------------===//

#include "src/sys/socket/getpeername.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/getpeername.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/struct_sockaddr.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, getpeername,
                   (int sockfd, struct sockaddr *addr, socklen_t *addrlen)) {
  auto result = windows_syscalls::getpeername(sockfd, addr, addrlen);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
