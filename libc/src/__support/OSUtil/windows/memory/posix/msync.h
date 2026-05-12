//===- msync.h - POSIX-layer msync declaration ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `internal::msync` — file-backed mapping flush, optionally with
/// CoW-revert pre-pass for `MS_INVALIDATE`. Returns 0 on success,
/// -errno on failure (Linux syscall convention).
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MSYNC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MSYNC_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t msync(void *addr, size_t length, int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MSYNC_H
