//===-- Windows implementation of getgrnam_r ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/grp/getgrnam_r.h"
#include "include/llvm-libc-types/struct_group.h"
#include "src/__support/OSUtil/windows/process/group_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX getgrnam_r: returns 0 with *result=nullptr for not-found.
LLVM_LIBC_FUNCTION(int, getgrnam_r,
                   (const char *name, struct group *grp, char *buf,
                    size_t buflen, struct group **result)) {
  *result = nullptr;
  int err = internal::fill_group_name(name, grp, buf, buflen);
  if (err == ENOENT)
    return 0;
  if (err == 0)
    *result = grp;
  return err;
}

} // namespace LIBC_NAMESPACE_DECL
