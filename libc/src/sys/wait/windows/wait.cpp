//===-- Windows implementation of wait -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/wait/wait.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/sys/wait/waitpid.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(pid_t, wait, (int *waitstatus)) {
  return LIBC_NAMESPACE::waitpid(-1, waitstatus, 0);
}

} // namespace LIBC_NAMESPACE_DECL
