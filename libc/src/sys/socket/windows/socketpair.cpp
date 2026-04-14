//===-- Windows implementation of socketpair ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::socketpair() in socket_engine.h.
//
//===----------------------------------------------------------------------===//

#include "src/sys/socket/socketpair.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/socketpair.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, socketpair,
                   (int domain, int type, int protocol, int sv[2])) {
  auto result = windows_syscalls::socketpair(domain, type, protocol, sv);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
