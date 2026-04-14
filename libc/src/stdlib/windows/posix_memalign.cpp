//===-- POSIX posix_memalign for Windows -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/posix_memalign.h"
#include "posix_alloc.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, posix_memalign,
                   (void **memptr, size_t alignment, size_t size)) {
  // POSIX.1-2024: alignment must be a power of two and a multiple of
  // sizeof(void *).
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;

  // POSIX.1-2024: posix_memalign shall not modify errno.
  int saved_errno = libc_errno;

  void *ptr = posix_alloc_aligned(size, alignment);
  if (!ptr) {
    libc_errno = saved_errno;
    return ENOMEM;
  }

  *memptr = ptr;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
