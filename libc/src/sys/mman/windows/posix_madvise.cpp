//===---------- Windows implementation of POSIX posix_madvise function ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point -- delegates to internal::posix_madvise() in
// posix_madvise_ops.cpp.
//
// SPECIAL: posix_madvise returns the error code directly (POSIX spec).
// No libc_errno assignment.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/posix_madvise.h"

#include "src/__support/OSUtil/windows/memory/posix_madvise_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, posix_madvise, (void *addr, size_t len, int advice)) {
  intptr_t ret = internal::posix_madvise(addr, len, advice);
  if (ret < 0)
    return static_cast<int>(-ret);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
