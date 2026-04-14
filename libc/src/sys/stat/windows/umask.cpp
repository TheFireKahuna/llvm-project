//===---------- Windows implementation of the POSIX umask function --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to windows_sec::set_umask() via wrapper.
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/umask.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/umask.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(mode_t, umask, (mode_t mask)) {
  return windows_syscalls::umask(mask).value();
}

} // namespace LIBC_NAMESPACE_DECL
