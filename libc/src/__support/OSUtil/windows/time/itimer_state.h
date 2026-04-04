//===-- Process-wide interval timer state -------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_ITIMER_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_ITIMER_STATE_H

#include "hdr/types/struct_itimerval.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct ItimerState {
  RawMutex lock;
  HANDLE timer;
  int64_t deadline_qpc;
  int64_t qpc_freq;
  struct itimerval current;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_ITIMER_STATE_H
