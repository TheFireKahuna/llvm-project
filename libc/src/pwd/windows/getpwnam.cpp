//===-- Windows implementation of getpwnam --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/pwd/getpwnam.h"
#include "include/llvm-libc-types/struct_passwd.h"
#include "src/__support/OSUtil/windows/process/passwd_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX getpwnam: returns a pointer to a static struct passwd, or nullptr.
// On not-found, returns nullptr without changing errno. On error, sets errno.
LLVM_LIBC_FUNCTION(struct passwd *, getpwnam, (const char *name)) {
  static thread_local struct passwd pwd;
  static thread_local char buf[1024];

  int err = internal::fill_passwd_name(name, &pwd, buf, sizeof(buf));
  if (err == ENOENT)
    return nullptr;
  if (err != 0) {
    libc_errno = err;
    return nullptr;
  }
  return &pwd;
}

} // namespace LIBC_NAMESPACE_DECL
