//===-- Internal ownership operations for Windows ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_OWNERSHIP_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_OWNERSHIP_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Returns 0 on success, -errno on failure.
intptr_t chown(const char *path, uid_t owner, gid_t group);

/// Returns 0 on success, -errno on failure.
intptr_t fchown(int fd, uid_t owner, gid_t group);

/// Returns 0 on success, -errno on failure.
intptr_t fchownat(int dfd, const char *path, uid_t owner, gid_t group, int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_OWNERSHIP_OPS_H
