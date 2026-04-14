//===-- Internal stat/lstat/fstat/fstatat declarations -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: stat functions that implement Linux
// syscall semantics (return 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STAT_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STAT_OPS_H

#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-types/struct_stat.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t stat(const char *__restrict path, struct stat *__restrict statbuf);
intptr_t lstat(const char *__restrict path, struct stat *__restrict statbuf);
intptr_t fstat(int fd, struct stat *statbuf);
intptr_t fstatat(int dfd, const char *__restrict path,
             struct stat *__restrict statbuf, int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STAT_OPS_H
