//===-- windows_syscalls::posix_spawnp() wrapper ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PATH-searching variant of posix_spawn. Delegates to the shared
// search_path_for_exec in process_utils.h, which searches each PATH
// directory trying both the bare name and with ".exe" appended.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_POSIX_SPAWNP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_POSIX_SPAWNP_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/process/process_utils.h"
#include "src/__support/OSUtil/windows/process/spawn_ops.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

namespace {

struct SpawnpPathCtx {
  pid_t *pid;
  const posix_spawn_file_actions_t *file_actions;
  const posix_spawnattr_t *attr;
  char *const *argv;
  char *const *envp;
};

intptr_t spawnp_path_cb(const char *candidate, void *ctx) {
  auto *c = static_cast<SpawnpPathCtx *>(ctx);
  return internal::posix_spawn(c->pid, candidate, c->file_actions, c->attr,
                               c->argv, c->envp);
}

} // anonymous namespace

LIBC_INLINE ErrorOr<int>
posix_spawnp(pid_t *__restrict pid, const char *__restrict file,
             const posix_spawn_file_actions_t *file_actions,
             const posix_spawnattr_t *__restrict attr,
             char *const *__restrict argv, char *const *__restrict envp) {
  SpawnpPathCtx ctx{pid, file_actions, attr, argv, envp};
  intptr_t ret = process_utils::search_path_for_exec(file, spawnp_path_cb,
                                                     &ctx);
  if (ret < 0)
    return Error(-static_cast<int>(ret));
  return 0;
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_POSIX_SPAWNP_H
