//===-- Internal exec operation declarations ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: exec functions that implement
// POSIX exec semantics via self-hollowing (PID-preserving image replacement).
// Returns -errno on failure; does not return on success.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_EXEC_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_EXEC_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Replace the current process image with the program at `path`.
// argv/envp follow POSIX exec conventions.
// Returns -errno on failure. Does not return on success.
intptr_t execve(const char *path, char *const argv[], char *const envp[]);

// Search PATH for executable then call execve.
// Returns -errno on failure. Does not return on success.
intptr_t execvp(const char *file, char *const argv[]);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_EXEC_OPS_H
