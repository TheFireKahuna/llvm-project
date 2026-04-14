//===-- passwd database operations for Windows (NT-POSIX) ---- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal engine functions for getpwuid / getpwnam. These fill a caller-
// supplied struct passwd + string buffer, returning 0 on success or a
// positive errno value on failure.
//
// The implementation queries the Windows ProfileList registry key to resolve
// user names and home directories from SIDs. The primary group comes from the
// process token for the current user, or defaults to the uid for other users.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PASSWD_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PASSWD_OPS_H

#include "hdr/types/gid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/uid_t.h"
#include "include/llvm-libc-types/struct_passwd.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Fill *pwd by looking up the user with the given uid.
// String data is written into buf[0..buflen-1]; pointers in *pwd point there.
// Returns 0 on success, positive errno on failure (ENOENT, ERANGE, etc.).
int fill_passwd_uid(uid_t uid, struct passwd *pwd, char *buf, size_t buflen);

// Fill *pwd by looking up the user with the given name.
// String data is written into buf[0..buflen-1]; pointers in *pwd point there.
// Returns 0 on success, positive errno on failure (ENOENT, ERANGE, etc.).
int fill_passwd_name(const char *name, struct passwd *pwd, char *buf,
                     size_t buflen);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PASSWD_OPS_H
