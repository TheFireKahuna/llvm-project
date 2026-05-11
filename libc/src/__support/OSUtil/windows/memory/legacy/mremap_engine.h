//===-- Internal mremap declaration ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::mremap() implemented in mremap_engine.cpp.
// Returns the mapped address as an intptr_t on success, or -errno on failure.
// intptr_t (not long) because Windows LLP64 has 4-byte long / 8-byte pointers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MREMAP_ENGINE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MREMAP_ENGINE_H

#include "src/__support/macros/config.h"
#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mremap(void *old_addr, size_t old_size, size_t new_size, int flags,
                void *new_addr);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MREMAP_ENGINE_H
