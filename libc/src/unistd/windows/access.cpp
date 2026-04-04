//===-- Windows implementation of access ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/access.h"

#include "hdr/fcntl_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int faccessat(int dfd, const char *path, int amode, int flag);

LLVM_LIBC_FUNCTION(int, access, (const char *path, int mode)) {
  return faccessat(AT_FDCWD, path, mode, 0);
}

} // namespace LIBC_NAMESPACE_DECL
