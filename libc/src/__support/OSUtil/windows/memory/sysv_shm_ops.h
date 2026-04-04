//===-- Internal SysV shared memory declarations ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: SysV shared memory functions.
// All functions return value on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SYSV_SHM_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SYSV_SHM_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/key_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/struct_shmid_ds.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Create or look up a SysV shared memory segment.
/// Returns shmid on success, -errno on failure.
intptr_t shmget(key_t key, size_t size, int shmflg);

/// Attach a SysV shared memory segment to the calling process.
/// Returns mapped address as intptr_t on success, -errno on failure.
intptr_t shmat(int shmid, const void *shmaddr, int shmflg);

/// Detach a SysV shared memory segment from the calling process.
/// Returns 0 on success, -errno on failure.
intptr_t shmdt(const void *shmaddr);

/// Perform control operations on a SysV shared memory segment.
/// Returns 0 on success (or value for specific cmds), -errno on failure.
intptr_t shmctl(int shmid, int cmd, struct shmid_ds *buf);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SYSV_SHM_OPS_H
