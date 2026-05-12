//===-- Internal mlock/munlock declarations --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: mlock family functions that
// implement Linux syscall semantics (return 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MLOCK_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MLOCK_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// mlock / mlock2 / munlock now live in
// src/__support/OSUtil/windows/memory/posix/mlock.h. Only the
// process-wide mlockall / munlockall paths remain in the legacy
// implementation until P9 rebuilds them on the new substrate.

intptr_t mlockall(int flags);
intptr_t munlockall();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MLOCK_OPS_H
