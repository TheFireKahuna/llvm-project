//===-- group database operations for Windows (NT-POSIX) ---- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal engine functions for getgrgid / getgrnam. These fill a caller-
// supplied struct group + string buffer, returning 0 on success or a
// positive errno value on failure.
//
// The implementation queries the process token's TokenGroups for group
// membership and uses a well-known group name table for builtin Windows
// groups. Unknown groups fall back to their SID string as the name.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_GROUP_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_GROUP_OPS_H

#include "hdr/types/gid_t.h"
#include "hdr/types/size_t.h"
#include "include/llvm-libc-types/struct_group.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Fill *grp by looking up the group with the given gid.
// String data is written into buf[0..buflen-1]; pointers in *grp point there.
// Returns 0 on success, positive errno on failure (ENOENT, ERANGE, etc.).
int fill_group_gid(gid_t gid, struct group *grp, char *buf, size_t buflen);

// Fill *grp by looking up the group with the given name.
// String data is written into buf[0..buflen-1]; pointers in *grp point there.
// Returns 0 on success, positive errno on failure (ENOENT, ERANGE, etc.).
int fill_group_name(const char *name, struct group *grp, char *buf,
                    size_t buflen);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_GROUP_OPS_H
