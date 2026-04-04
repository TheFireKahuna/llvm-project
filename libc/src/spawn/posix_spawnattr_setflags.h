//===-- Implementation header for posix_spawnattr_setflags -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_SETFLAGS_H
#define LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_SETFLAGS_H

#include "src/__support/macros/config.h"
#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_SETFLAGS_H
