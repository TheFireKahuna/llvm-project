//===---------- Windows implementation of mbind ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::mbind() in numa_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mbind.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/mbind.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(long, mbind,
                   (void *addr, unsigned long len, int mode,
                    const unsigned long *nodemask, unsigned long maxnode,
                    unsigned int flags)) {
  auto result =
      windows_syscalls::mbind(addr, len, mode, nodemask, maxnode, flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
