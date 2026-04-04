//===-- Implementation header for posix_spawnattr_getflags -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETFLAGS_H
#define LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETFLAGS_H

#include "src/__support/macros/config.h"
#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict attr,
                             short *__restrict flags);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETFLAGS_H
