//===-- Windows implementation of posix_fallocate -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/posix_fallocate.h"

#include "src/__support/OSUtil/windows/io/fallocate_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, posix_fallocate, (int fd, off_t offset, off_t len)) {
  return internal::posix_fallocate(fd, offset, len);
}

} // namespace LIBC_NAMESPACE_DECL
