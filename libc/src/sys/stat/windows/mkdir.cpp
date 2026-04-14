//===-- Windows implementation of mkdir -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/mkdir.h"

#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int mkdirat(int dfd, const char *path, mode_t mode);

LLVM_LIBC_FUNCTION(int, mkdir, (const char *path, mode_t mode)) {
  return mkdirat(AT_FDCWD, path, mode);
}

} // namespace LIBC_NAMESPACE_DECL
