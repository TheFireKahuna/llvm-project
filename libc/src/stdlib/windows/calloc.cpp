//===-- POSIX calloc for Windows -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/calloc.h"
#include "posix_alloc.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void *, calloc, (size_t nelem, size_t size)) {
  // C17 7.22.3.2: detect multiplication overflow.
  size_t total;
  if (__builtin_mul_overflow(nelem, size, &total)) {
    libc_errno = ENOMEM;
    return nullptr;
  }
  return posix_alloc_zeroed(total);
}

} // namespace LIBC_NAMESPACE_DECL
