//===-- POSIX aligned_alloc for Windows ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/aligned_alloc.h"
#include "posix_alloc.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void *, aligned_alloc, (size_t alignment, size_t size)) {
  // C11 7.22.3.1: alignment must be a power of two, size a multiple.
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      size % alignment != 0) {
    libc_errno = EINVAL;
    return nullptr;
  }

  return posix_alloc_aligned(size, alignment);
}

} // namespace LIBC_NAMESPACE_DECL
