//===-- Internal mmap declaration -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::mmap() implemented in mmap_engine.cpp.
// Returns the mapped address as an intptr_t on success, or -errno on failure.
// intptr_t (not long) because Windows LLP64 has 4-byte long / 8-byte pointers.
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

intptr_t mmap(void *addr, size_t size, int prot, int flags, int fd,
              off_t offset);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MMAP_ENGINE_H
