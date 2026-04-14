//===-- Windows implementation of rmdir -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/rmdir.h"

#include "hdr/fcntl_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int unlinkat(int dfd, const char *path, int flags);

LLVM_LIBC_FUNCTION(int, rmdir, (const char *path)) {
  return unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

} // namespace LIBC_NAMESPACE_DECL
