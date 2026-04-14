//===-- Inotify operations for NT-POSIX --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INOTIFY_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INOTIFY_OPS_H

#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-types/ssize_t.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription;

/// Create an inotify instance. flags may include IN_NONBLOCK and IN_CLOEXEC.
/// Returns fd on success, -errno on failure.
intptr_t inotify_init1(int flags);

/// Add or modify a watch. Returns wd on success, -errno on failure.
intptr_t inotify_add_watch(int fd, const char *pathname, uint32_t mask);

/// Remove a watch. Returns 0 on success, -errno on failure.
intptr_t inotify_rm_watch(int fd, int wd);

/// Core inotify read logic operating on an already-validated OFD.
/// Called via FileOps inotify_read_impl().
ssize_t inotify_read_ofd(OpenFileDescription *ofd, void *buf, size_t count);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INOTIFY_OPS_H
