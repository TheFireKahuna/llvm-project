//===- mlock.h - POSIX-layer mlock/mlock2/munlock declarations --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Page-locking entry points. Each returns 0 on success, negative errno
// on failure (Linux syscall convention); the public `mlock` / `mlock2` /
// `munlock` shims flip the sign before storing to `errno`.
intptr_t mlock(const void *addr, size_t len);
intptr_t mlock2(const void *addr, size_t len, int flags);
intptr_t munlock(const void *addr, size_t len);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCK_H
