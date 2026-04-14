//===-- Virtual special-files namespace -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_VIRTUAL_FS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_VIRTUAL_FS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription;

namespace virtual_fs {

enum class NodeKind : uint8_t {
  Invalid = 0,
  DevRoot,
  PtsDir,
  Ptmx,
  PtsSlave,
};

ErrorOr<int> openat(int dirfd, const char *path, int flags);

bool is_virtual_dir(const OpenFileDescription *ofd);
NodeKind directory_kind(const OpenFileDescription *ofd);

void release_opaque(void *);

} // namespace virtual_fs
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_VIRTUAL_FS_H
