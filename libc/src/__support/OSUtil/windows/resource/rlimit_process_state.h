//===-- Resource limit process state for Windows ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_PROCESS_STATE_H

#include "hdr/types/struct_rlimit.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

inline constexpr int RLIMIT_PROCESS_COUNT = 16;

struct RlimitProcessState {
  HANDLE job;
  struct rlimit limits[RLIMIT_PROCESS_COUNT];
  DWORD active_flags;
  bool cpu_soft_rearming;
  cpp::Atomic<int> initialized;
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RLIMIT_PROCESS_STATE_H
