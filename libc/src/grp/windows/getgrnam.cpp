//===-- Windows implementation of getgrnam --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/grp/getgrnam.h"
#include "include/llvm-libc-types/struct_group.h"
#include "src/__support/OSUtil/windows/process/group_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX getgrnam: returns nullptr without changing errno on not-found.
LLVM_LIBC_FUNCTION(struct group *, getgrnam, (const char *name)) {
  static thread_local struct group grp;
  static thread_local char buf[1024];

  int err = internal::fill_group_name(name, &grp, buf, sizeof(buf));
  if (err == ENOENT)
    return nullptr;
  if (err != 0) {
    libc_errno = err;
    return nullptr;
  }
  return &grp;
}

} // namespace LIBC_NAMESPACE_DECL
