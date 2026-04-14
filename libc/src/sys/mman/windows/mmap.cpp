//===---------- Windows implementation of the POSIX mmap function ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to internal::mmap() in mmap_engine.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"

#include "src/__support/OSUtil/windows/resource/rlimit_data_guard.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/mmap.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void *, mmap,
                   (void *addr, size_t size, int prot, int flags, int fd,
                    off_t offset)) {
  windows::ScopedRlimitDataPublicCall rlimit_scope;
  if (!windows::allows_public_rlimit_data_growth(size)) {
    libc_errno = ENOMEM;
    return MAP_FAILED;
  }

  auto result = windows_syscalls::mmap(addr, size, prot, flags, fd, offset);
  if (!result.has_value()) {
    libc_errno = result.error();
    return MAP_FAILED;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
