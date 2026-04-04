//===-- Internal environment variable operations ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal functions implementing POSIX environment variable semantics
// (getenv/setenv/unsetenv/putenv) against the PCB's EnvironmentState.
//
// All functions are thread-safe — they acquire g_pcb.environment.lock internally.
// The POSIX entrypoints in src/stdlib/ are thin wrappers around these.
// Unlike the I/O ops (which return -errno), these set libc_errno directly
// since they're purely userspace with no syscall translation layer.
//
// Ownership tracking: the env_ptrs allocation is a combined block:
//
//   [ char*[capacity] | uint8_t[(capacity+7)/8] ]
//     ^                  ^
//     g_pcb.environment.ptrs     ownership bitmap
//
// Bit set in the bitmap = we allocated the string (setenv / build_environ).
// Bit clear = caller owns the string (putenv). Only owned strings are freed
// on overwrite or removal.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_ENV_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_ENV_OPS_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Get value for name. Thread-safe (acquires env_lock).
// Returns pointer to the value portion (after '='), or nullptr if not found.
// On Windows, variable names are matched case-insensitively.
// The returned pointer is valid until the next env mutation.
char *env_get(const char *name);

// Set name=value. Thread-safe (acquires env_lock).
// If overwrite is 0 and name already exists, the value is not changed.
// On Windows, variable names are matched case-insensitively.
// Allocates a new "NAME=VALUE" string via malloc (marked as owned).
// Returns 0 on success, -1 on error with errno set (EINVAL or ENOMEM).
int env_set(const char *name, const char *value, int overwrite);

// Remove ALL entries matching name. Thread-safe (acquires env_lock).
// On Windows, variable names are matched case-insensitively.
// Compacts the array by shifting entries down. Frees owned strings.
// Returns 0 on success, -1 on error with errno set (EINVAL).
int env_unset(const char *name);

// Insert a pre-formed "NAME=VALUE" string. Thread-safe (acquires env_lock).
// The string is NOT copied — caller retains ownership (marked as unowned).
// If name already exists, the old entry is replaced (old owned string freed).
// On Windows, variable names are matched case-insensitively.
// Returns 0 on success, -1 on error with errno set (EINVAL or ENOMEM).
int env_put(char *string);

// Clear all environment variables. Thread-safe (acquires env_lock).
// Frees all owned strings, sets environ to an empty array.
// Returns 0 on success.
int env_clear();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_ENV_OPS_H
