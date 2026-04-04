//===-- Internal statvfs declarations ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STATVFS_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STATVFS_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

struct statvfs;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t statvfs_from_handle(void *handle, struct statvfs *buf);
intptr_t statvfs(const char *__restrict path, struct statvfs *__restrict buf);
intptr_t fstatvfs(int fd, struct statvfs *buf);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STATVFS_OPS_H
