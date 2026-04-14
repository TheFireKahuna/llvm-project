//===-- Windows implementation of symlink ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/symlink.h"

#include "hdr/fcntl_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int symlinkat(const char *target, int newdfd, const char *linkpath);

LLVM_LIBC_FUNCTION(int, symlink,
                   (const char *target, const char *linkpath)) {
  return symlinkat(target, AT_FDCWD, linkpath);
}

} // namespace LIBC_NAMESPACE_DECL
