//===-- Windows implementation of link ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/link.h"

#include "hdr/fcntl_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int linkat(int olddfd, const char *oldpath, int newdfd, const char *newpath,
           int flags);

LLVM_LIBC_FUNCTION(int, link, (const char *oldpath, const char *newpath)) {
  // POSIX link() follows symlinks (AT_SYMLINK_FOLLOW).
  return linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, AT_SYMLINK_FOLLOW);
}

} // namespace LIBC_NAMESPACE_DECL
