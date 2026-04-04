//===-- Windows implementation of getpwuid_r ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/pwd/getpwuid_r.h"
#include "include/llvm-libc-types/struct_passwd.h"
#include "src/__support/OSUtil/windows/process/passwd_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX getpwuid_r: reentrant version. Returns 0 on success or not-found
// (with *result = nullptr for not-found). Returns a positive errno value
// only for real errors (ERANGE, EIO, etc.).
LLVM_LIBC_FUNCTION(int, getpwuid_r,
                   (uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
                    struct passwd **result)) {
  *result = nullptr;
  int err = internal::fill_passwd_uid(uid, pwd, buf, buflen);
  if (err == ENOENT)
    return 0; // Not found: success with *result = nullptr.
  if (err == 0)
    *result = pwd;
  return err;
}

} // namespace LIBC_NAMESPACE_DECL
