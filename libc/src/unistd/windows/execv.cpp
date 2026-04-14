//===-- Windows implementation of execv ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/execv.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/unistd/environ.h"
#include "src/unistd/execve.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, execv, (const char *path, char *const argv[])) {
  return LIBC_NAMESPACE::execve(path, argv, LIBC_NAMESPACE::environ);
}

} // namespace LIBC_NAMESPACE_DECL
