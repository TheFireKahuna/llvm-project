//===-- IPC process state for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_IPC_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_IPC_PROCESS_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct IpcProcessState {
  cpp::Atomic<uint64_t> fifo_siphash_k0;
  cpp::Atomic<uint64_t> fifo_siphash_k1;
  cpp::Atomic<uint32_t> fifo_siphash_init;
  cpp::Atomic<uint32_t> epoll_router_installed;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_IPC_PROCESS_STATE_H
