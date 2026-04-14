//===-- Internal fallocate operation declarations ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::posix_fallocate. Returns 0 on success or
// a positive errno value on failure (matching the POSIX convention where
// posix_fallocate does not set errno).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FALLOCATE_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FALLOCATE_OPS_H

#include "hdr/types/off_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns 0 on success, positive errno on failure.
// posix_fallocate has unusual POSIX convention: returns error code directly.
int posix_fallocate(int fd, off_t offset, off_t len);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FALLOCATE_OPS_H
