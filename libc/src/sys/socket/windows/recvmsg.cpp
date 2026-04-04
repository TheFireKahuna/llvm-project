//===-- Windows implementation of recvmsg ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::recvmsg() in socket_engine.h.
//
//===----------------------------------------------------------------------===//

#include "src/sys/socket/recvmsg.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/recvmsg.h"
#include "hdr/types/struct_msghdr.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, recvmsg,
                   (int sockfd, struct msghdr *msg, int flags)) {
  auto result = windows_syscalls::recvmsg(sockfd, msg, flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
