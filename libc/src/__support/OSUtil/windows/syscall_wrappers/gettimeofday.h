//===-- windows_syscalls::gettimeofday() wrapper ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETTIMEOFDAY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETTIMEOFDAY_H

#include "hdr/time_macros.h"
#include "hdr/types/suseconds_t.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/clock_gettime.h"
#include "src/__support/time/units.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<int> gettimeofday(struct timeval *tv,
                                      [[maybe_unused]] void *unused) {
  using namespace time_units;
  if (tv == nullptr)
    return 0;

  struct timespec ts;
  auto result = internal::clock_gettime(CLOCK_REALTIME, &ts);
  if (!result.has_value())
    return Error(result.error());

  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = static_cast<suseconds_t>(ts.tv_nsec / 1_us_ns);
  return 0;
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_GETTIMEOFDAY_H
