//===- mincore.h - POSIX-layer mincore declaration --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `internal::mincore` — POSIX page-residency probe. Returns 0 on
/// success, -errno on failure (Linux syscall convention). The body in
/// `mincore.cpp` is a `RegionWalker` over the requested range invoking
/// `nt_pal::query_working_set_ex` per committed chunk.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MINCORE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MINCORE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mincore(void *addr, size_t length, unsigned char *vec);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MINCORE_H
