//===-- Internal identity operations for Windows -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_IDENTITY_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_IDENTITY_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Get the login name for the current effective user. Writes the UTF-8
/// username (domain prefix stripped) into buf. Returns the length on
/// success, or -errno on failure (-ERANGE if bufsize is too small,
/// -ENOMEM if SID conversion fails).
intptr_t getlogin(char *buf, size_t bufsize);

/// Returns 0 on success, -errno on failure.
intptr_t setuid(uid_t uid);

/// Returns 0 on success, -errno on failure.
intptr_t seteuid(uid_t euid);

/// Set real and/or effective UID atomically (pass -1 to leave unchanged).
/// If the real UID is set, the saved-set-UID is also set to the new
/// effective UID per POSIX. Returns 0 on success, -errno on failure.
intptr_t setreuid(uid_t ruid, uid_t euid);

/// Get supplementary group list. If size is 0, returns the count of
/// supplementary groups. If size > 0, fills list[] and returns the count.
/// Returns -errno on failure (e.g. -EINVAL if size < actual count).
intptr_t getgroups(int size, gid_t list[]);

/// Returns 0 on success, -errno on failure.
intptr_t setgid(gid_t gid);

/// Returns 0 on success, -errno on failure.
intptr_t setegid(gid_t egid);

/// Set real and/or effective GID atomically (pass -1 to leave unchanged).
/// If the real GID is set, the saved-set-GID is also set to the new
/// effective GID per POSIX. Returns 0 on success, -errno on failure.
intptr_t setregid(gid_t rgid, gid_t egid);

/// Get all three UIDs atomically. Returns 0 on success, -errno on failure.
intptr_t getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);

/// Get all three GIDs atomically. Returns 0 on success, -errno on failure.
intptr_t getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);

/// Set any combination of real, effective, and saved UID (pass -1 to leave
/// unchanged). Unprivileged: each specified ID must equal one of the current
/// three. Returns 0 on success, -errno on failure.
intptr_t setresuid(uid_t ruid, uid_t euid, uid_t suid);

/// Set any combination of real, effective, and saved GID (pass -1 to leave
/// unchanged). Unprivileged: each specified ID must equal one of the current
/// three. Returns 0 on success, -errno on failure.
intptr_t setresgid(gid_t rgid, gid_t egid, gid_t sgid);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_IDENTITY_OPS_H
