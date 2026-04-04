//===-- Windows implementation of getlogin_r -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getlogin_r.h"
#include "src/__support/OSUtil/windows/process/identity_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX getlogin_r: returns 0 on success, errno on failure (not -1).
LLVM_LIBC_FUNCTION(int, getlogin_r, (char *buf, size_t bufsize)) {
  intptr_t ret = internal::getlogin(buf, bufsize);
  if (ret < 0)
    return static_cast<int>(-ret);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
