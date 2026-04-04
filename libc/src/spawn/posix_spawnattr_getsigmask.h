//===-- Implementation header for posix_spawnattr_getsigmask ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETSIGMASK_H
#define LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETSIGMASK_H

#include "src/__support/macros/config.h"
#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

int posix_spawnattr_getsigmask(const posix_spawnattr_t *__restrict attr,
                               sigset_t *__restrict sigmask);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SPAWN_POSIX_SPAWNATTR_GETSIGMASK_H
