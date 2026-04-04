//===-- Internal realpath declaration ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::realpath that implements Linux syscall
// semantics (returns intptr_t pointer on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_REALPATH_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_REALPATH_OPS_H

#include "src/__support/macros/config.h"
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t realpath(const char *__restrict path, char *__restrict resolved_path);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_REALPATH_OPS_H
