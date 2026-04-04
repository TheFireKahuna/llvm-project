//===-- Exec process state for Windows --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_EXEC_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_EXEC_PROCESS_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct ExecProcessState {
  cpp::Atomic<HANDLE *> handles;
  cpp::Atomic<int> max_fd;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_EXEC_PROCESS_STATE_H
