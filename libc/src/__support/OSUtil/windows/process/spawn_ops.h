//===-- Internal posix_spawn declaration --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::posix_spawn that implements Linux syscall
// semantics (return 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SPAWN_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SPAWN_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t posix_spawn(pid_t *__restrict pid, const char *__restrict path,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *__restrict attr,
                 char *const *__restrict argv,
                 char *const *__restrict envp,
                 const char *raw_cmdline = nullptr);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SPAWN_OPS_H
