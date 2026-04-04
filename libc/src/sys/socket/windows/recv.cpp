//===-- Windows implementation of recv ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::recv() in socket_engine.h.
//
//===----------------------------------------------------------------------===//

#include "src/sys/socket/recv.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/recv.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(ssize_t, recv,
                   (int sockfd, void *buf, size_t len, int flags)) {
  auto result = windows_syscalls::recv(sockfd, buf, len, flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
