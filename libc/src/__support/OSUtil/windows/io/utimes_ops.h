//===-- Internal utimes/lutimes/futimes declarations -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: utimes functions that implement
// Linux syscall semantics (return 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_UTIMES_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_UTIMES_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

struct timeval;
struct timespec;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t utimes(const char *path, const struct timeval times[2]);
intptr_t lutimes(const char *path, const struct timeval times[2]);
intptr_t futimes(int fd, const struct timeval times[2]);
intptr_t utimensat(int dirfd, const char *path,
                   const struct timespec times[2], int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_UTIMES_OPS_H
