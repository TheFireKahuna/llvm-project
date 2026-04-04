//===-- Windows implementation of lchown -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/lchown.h"

#include "hdr/fcntl_macros.h"
#include "src/unistd/fchownat.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, lchown,
                   (const char *path, uid_t owner, gid_t group)) {
  return LIBC_NAMESPACE::fchownat(AT_FDCWD, path, owner, group,
                                  AT_SYMLINK_NOFOLLOW);
}

} // namespace LIBC_NAMESPACE_DECL
