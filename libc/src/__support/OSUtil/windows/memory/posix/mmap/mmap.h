//===- mmap.h - POSIX-layer mmap declaration --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `internal::mmap` — the top-level POSIX dispatcher. Validates the
/// flag matrix, then routes by shape to one of the per-shape handlers.
/// Anonymous-private is the only shape fully implemented in P2; the
/// remaining shapes return `-ENOSYS` pending P3 (MAP_FIXED) and P4
/// (file-backed, shared, hugetlb).
///
/// `internal::mmap_anon_private` is exposed for direct callers (and
/// for tests that target the new path without going through the full
/// dispatcher).
///
/// Both return the mapped address as an `intptr_t` on success and a
/// Linux-flavoured `-errno` on failure.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/off_t.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mmap(void *addr, size_t size, int prot, int flags, int fd,
              off_t offset);

intptr_t mmap_anon_private(void *addr, size_t size, int prot, int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MMAP_MMAP_H
