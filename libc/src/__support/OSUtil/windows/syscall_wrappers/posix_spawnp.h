//===-- windows_syscalls::posix_spawnp() wrapper ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PATH-searching variant of posix_spawn. Searches each PATH directory for
// the executable, trying both the bare name and with ".exe" appended (Windows
// convention). Delegates to internal::posix_spawn for each candidate.
//
// PATH is read from the caller's environment (environ), not from envp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_POSIX_SPAWNP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_POSIX_SPAWNP_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/size_t.h"
#include "src/__support/OSUtil/windows/io/env_ops.h"
#include "src/__support/OSUtil/windows/process/spawn_ops.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

namespace {

constexpr int SPAWNP_MAX_PATH = 1024;

inline bool spawnp_has_dir_separator(const char *file) {
  for (const char *p = file; *p; ++p) {
    if (*p == '/' || *p == '\\')
      return true;
  }
  return false;
}

inline bool spawnp_has_extension(const char *file) {
  const char *dot = nullptr;
  for (const char *p = file; *p; ++p) {
    if (*p == '.')
      dot = p;
    else if (*p == '/' || *p == '\\')
      dot = nullptr;
  }
  return dot != nullptr;
}

inline const char *spawnp_find_path_env() {
  return LIBC_NAMESPACE::internal::env_get("PATH");
}

inline int spawnp_build_candidate(char *buf, int buf_size, const char *dir,
                                  size_t dir_len, const char *file,
                                  size_t file_len) {
  int pos = 0;
  for (size_t i = 0; i < dir_len; ++i) {
    if (pos >= buf_size - 1)
      return 0;
    buf[pos++] = dir[i];
  }
  if (pos > 0 && buf[pos - 1] != '\\' && buf[pos - 1] != '/') {
    if (pos >= buf_size - 1)
      return 0;
    buf[pos++] = '\\';
  }
  for (size_t i = 0; i < file_len; ++i) {
    if (pos >= buf_size - 1)
      return 0;
    buf[pos++] = file[i];
  }
  buf[pos] = '\0';
  return pos;
}

inline size_t spawnp_strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    ++len;
  return len;
}

} // anonymous namespace

LIBC_INLINE ErrorOr<int>
posix_spawnp(pid_t *__restrict pid, const char *__restrict file,
             const posix_spawn_file_actions_t *file_actions,
             const posix_spawnattr_t *__restrict attr,
             char *const *__restrict argv, char *const *__restrict envp) {
  if (!file || !file[0])
    return Error(ENOENT);

  // If file contains a directory separator, use it directly — no PATH search.
  if (spawnp_has_dir_separator(file)) {
    intptr_t ret =
        internal::posix_spawn(pid, file, file_actions, attr, argv, envp);
    if (ret < 0)
      return Error(-static_cast<int>(ret));
    return 0;
  }

  const char *path_env = spawnp_find_path_env();

  // No PATH — try file as-is (current directory).
  if (!path_env || !path_env[0]) {
    intptr_t ret =
        internal::posix_spawn(pid, file, file_actions, attr, argv, envp);
    if (ret < 0)
      return Error(-static_cast<int>(ret));
    return 0;
  }

  size_t file_len = spawnp_strlen(file);
  bool try_exe = !spawnp_has_extension(file);
  int last_errno = ENOENT;

  const char *p = path_env;
  while (true) {
    const char *end = p;
    while (*end && *end != ';')
      ++end;

    size_t dir_len = static_cast<size_t>(end - p);

    char candidate[SPAWNP_MAX_PATH];
    int cand_len = spawnp_build_candidate(candidate, SPAWNP_MAX_PATH, p,
                                          dir_len, file, file_len);

    if (cand_len > 0) {
      intptr_t ret =
          internal::posix_spawn(pid, candidate, file_actions, attr, argv, envp);
      if (ret >= 0)
        return 0; // Success.
      last_errno = static_cast<int>(-ret);

      // If not found and no extension, try appending ".exe".
      if (try_exe && (last_errno == ENOENT || last_errno == ENOTDIR) &&
          cand_len + 4 < SPAWNP_MAX_PATH) {
        candidate[cand_len] = '.';
        candidate[cand_len + 1] = 'e';
        candidate[cand_len + 2] = 'x';
        candidate[cand_len + 3] = 'e';
        candidate[cand_len + 4] = '\0';

        ret = internal::posix_spawn(pid, candidate, file_actions, attr, argv,
                                    envp);
        if (ret >= 0)
          return 0; // Success.
        last_errno = static_cast<int>(-ret);
      }

      // Stop searching on errors other than "not found".
      if (last_errno != ENOENT && last_errno != ENOTDIR)
        break;
    }

    if (!*end)
      break;
    p = end + 1;
  }

  return Error(last_errno);
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_POSIX_SPAWNP_H
