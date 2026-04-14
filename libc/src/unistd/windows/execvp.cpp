//===-- Windows implementation of execvp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::execvp() in exec_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/execvp.h"

#include "src/__support/OSUtil/windows/process/exec_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, execvp, (const char *file, char *const argv[])) {
  intptr_t ret = internal::execvp(file, argv);
  // execvp only returns on error.
  libc_errno = static_cast<int>(-ret);
  return -1;
}

} // namespace LIBC_NAMESPACE_DECL
