//===---------- Windows implementation of the POSIX mremap function -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::mremap() in mremap_engine.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mremap.h"

#include "src/__support/OSUtil/windows/resource/rlimit_data_guard.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/mremap.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include <stdarg.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void *, mremap,
                   (void *old_addr, size_t old_size, size_t new_size,
                    int flags, ...)) {
  windows::ScopedRlimitDataPublicCall rlimit_scope;
  void *new_addr = nullptr;
  if (flags & MREMAP_FIXED) {
    va_list ap;
    va_start(ap, flags);
    new_addr = va_arg(ap, void *);
    va_end(ap);
  }
  if (new_size > old_size &&
      !windows::allows_public_rlimit_data_growth(new_size - old_size)) {
    libc_errno = ENOMEM;
    return MAP_FAILED;
  }
  auto result = windows_syscalls::mremap(old_addr, old_size, new_size, flags,
                                         new_addr);
  if (!result.has_value()) {
    libc_errno = result.error();
    return MAP_FAILED;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
