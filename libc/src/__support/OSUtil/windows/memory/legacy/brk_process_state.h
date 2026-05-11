//===-- Program break state for Windows -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_BRK_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_BRK_PROCESS_STATE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/raw_mutex.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct BrkProcessState {
  char *base;
  cpp::Atomic<char *> current;
  char *reserved_end;
  size_t next_grow;
  RawMutex lock;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_BRK_PROCESS_STATE_H
