//===---------- Windows implementation of remap_file_pages ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::remap_file_pages() in
// remap_file_pages_ops.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/remap_file_pages.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/remap_file_pages.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, remap_file_pages,
                   (void *addr, size_t size, int prot, size_t pgoff,
                    int flags)) {
  auto result = windows_syscalls::remap_file_pages(addr, size, prot, pgoff,
                                                   flags);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
