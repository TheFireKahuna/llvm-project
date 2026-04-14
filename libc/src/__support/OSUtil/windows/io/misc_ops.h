//===-- Forward declarations for misc internal operations -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: misc operation functions that implement
// Linux syscall semantics (return value on success, -errno on failure).
//
// For sysconf/pathconf/fpathconf, the internal:: functions return ErrorOr<long>
// directly because -1 is a valid success value (POSIX "indeterminate").
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_MISC_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_MISC_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t getentropy(void *buf, size_t len);
intptr_t gethostname(char *name, size_t len);
ErrorOr<long> sysconf(int name);
ErrorOr<long> pathconf(const char *path, int name);
ErrorOr<long> fpathconf(int fd, int name);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_MISC_OPS_H
