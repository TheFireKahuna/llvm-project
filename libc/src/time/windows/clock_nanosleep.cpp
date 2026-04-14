//===-- Windows implementation of clock_nanosleep -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/clock_nanosleep.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/clock_nanosleep.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX: returns 0 on success, or a positive error number directly (not via
// errno).
LLVM_LIBC_FUNCTION(int, clock_nanosleep,
                   (clockid_t clockid, int flags, const timespec *req,
                    timespec *rem)) {
  cancel_check(); // POSIX cancellation point.
  auto result = windows_syscalls::clock_nanosleep(clockid, flags, req, rem);
  if (!result.has_value())
    return result.error();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
