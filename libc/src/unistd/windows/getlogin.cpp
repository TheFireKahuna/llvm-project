//===-- Windows implementation of getlogin ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getlogin.h"
#include "src/__support/OSUtil/windows/process/identity_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX LOGIN_NAME_MAX is typically 256. We use a thread-local static buffer
// so the returned pointer remains valid until the next call from the same
// thread.
static thread_local char login_buf[256];

LLVM_LIBC_FUNCTION(char *, getlogin, ()) {
  intptr_t ret = internal::getlogin(login_buf, sizeof(login_buf));
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return nullptr;
  }
  return login_buf;
}

} // namespace LIBC_NAMESPACE_DECL
