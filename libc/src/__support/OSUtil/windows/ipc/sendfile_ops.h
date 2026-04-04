//===-- Internal sendfile declaration ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::sendfile() — implements Linux syscall
// semantics (returns bytes sent on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SENDFILE_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SENDFILE_OPS_H

#include "hdr/types/off_t.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/macros/config.h"

#include "hdr/types/size_t.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns bytes sent (non-negative) or -errno on failure.
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SENDFILE_OPS_H
