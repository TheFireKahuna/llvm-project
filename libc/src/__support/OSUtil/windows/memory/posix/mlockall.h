//===- mlockall.h - POSIX-layer mlockall/munlockall declarations *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `internal::mlockall` / `internal::munlockall` — process-wide
/// page-locking entry points. Each returns 0 on success, -errno on
/// failure (Linux syscall convention).
///
/// `MCL_FUTURE` / `MCL_ONFAULT` flag bits live in `g_pcb.mlock.mcl_flags`
/// (PCB Zone 1, see `mlock_process_state.h`). `mmap` (P2) reads them
/// via `lock_if_future` to decide whether to lock newly-mapped ranges.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCKALL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCKALL_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mlockall(int flags);
intptr_t munlockall();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_MLOCKALL_H
