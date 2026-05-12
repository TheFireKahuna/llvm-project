//===-- Internal legacy_mmap_engine declaration -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for the legacy mmap implementation. The POSIX
// entry point `internal::mmap` now lives in `memory/posix/mmap/`; this
// engine is reached only via delegation from the new dispatcher for
// shapes the new tree has not yet implemented (file-backed, shared,
// MAP_FIXED, MAP_HUGETLB). Returns the mapped address as an intptr_t
// on success, or -errno on failure.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MMAP_ENGINE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MMAP_ENGINE_H

#include "src/__support/macros/config.h"
#include <stddef.h>
#include <stdint.h>

#include "hdr/types/off_t.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t legacy_mmap_engine(void *addr, size_t size, int prot, int flags,
                            int fd, off_t offset);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MMAP_ENGINE_H
