//===-- Linux implementation of clock_nanosleep ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/clock_nanosleep.h"
#include "hdr/stdint_proxy.h"
#include "hdr/time_macros.h"
#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <sys/syscall.h>

namespace LIBC_NAMESPACE_DECL {

// POSIX: returns 0 on success, or an error number directly (not via errno).
LLVM_LIBC_FUNCTION(int, clock_nanosleep,
                   (clockid_t clockid, int flags, const timespec *req,
                    timespec *rem)) {
#if defined(SYS_clock_nanosleep)
  int ret = LIBC_NAMESPACE::syscall_impl<int>(SYS_clock_nanosleep, clockid,
                                              flags, req, rem);
#elif defined(SYS_clock_nanosleep_time64)
  static_assert(
      sizeof(time_t) == sizeof(int64_t),
      "SYS_clock_nanosleep_time64 requires struct timespec with 64-bit "
      "members.");
  int ret = LIBC_NAMESPACE::syscall_impl<int>(SYS_clock_nanosleep_time64,
                                              clockid, flags, req, rem);
#else
#error "SYS_clock_nanosleep and SYS_clock_nanosleep_time64 not available."
#endif

  // clock_nanosleep returns positive error codes, not -1/errno.
  if (ret < 0)
    return -ret;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
