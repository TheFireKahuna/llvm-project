//===- munmap.h - POSIX-layer munmap declaration ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `internal::munmap` — release the VA between `addr` and `addr+size`
/// from the va_tracker. Returns 0 on success and a Linux-flavoured
/// `-errno` on failure. Holes inside the requested range are tolerated
/// (matches Linux); loaded PE image regions are rejected with
/// `-EINVAL`.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MUNMAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MUNMAP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t munmap(void *addr, size_t size);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MUNMAP_H
