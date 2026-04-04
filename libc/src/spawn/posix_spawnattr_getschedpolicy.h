//===-- Implementation header for posix_spawnattr_getschedpolicy -*- C++ -*===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETSCHEDPOLICY_H
#define LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETSCHEDPOLICY_H

#include "src/__support/macros/config.h"
#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *__restrict attr,
                                   int *__restrict policy);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETSCHEDPOLICY_H
