//===-- Windows implementation of posix_spawn ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/spawn/posix_spawn.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/posix_spawn.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, posix_spawn,
                   (pid_t *__restrict pid, const char *__restrict path,
                    const posix_spawn_file_actions_t *file_actions,
                    const posix_spawnattr_t *__restrict attr,
                    char *const *__restrict argv,
                    char *const *__restrict envp)) {
  auto result = windows_syscalls::posix_spawn(pid, path, file_actions,
                                              attr, argv, envp);
  if (!result.has_value())
    return result.error();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
