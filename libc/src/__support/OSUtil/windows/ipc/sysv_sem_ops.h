//===-- Internal SysV semaphore declarations --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: SysV semaphore functions.
// All functions return value on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SYSV_SEM_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SYSV_SEM_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/key_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/struct_sembuf.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t semget(key_t key, int nsems, int semflg);
intptr_t semctl(int semid, int semnum, int cmd, intptr_t cmd_arg);
intptr_t semop(int semid, struct sembuf *sops, size_t nsops);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SYSV_SEM_OPS_H
