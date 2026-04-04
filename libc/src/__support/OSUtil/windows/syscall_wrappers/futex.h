//===-- windows_syscalls::futex() wrapper -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_FUTEX_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_FUTEX_H

#include "hdr/stdint_proxy.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"
#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

LIBC_INLINE ErrorOr<long> futex(volatile uint32_t *uaddr, int futex_op,
                                uint32_t val, const struct timespec *timeout,
                                volatile uint32_t *uaddr2, uint32_t val3) {
  (void)uaddr2;

  static constexpr int FUTEX_WAIT = 0;
  static constexpr int FUTEX_WAKE = 1;
  static constexpr int FUTEX_WAIT_BITSET = 9;
  static constexpr int FUTEX_WAKE_BITSET = 10;
  static constexpr int FUTEX_PRIVATE_FLAG = 128;
  // FUTEX_CLOCK_REALTIME (0x100) is accepted as a no-op — our backend
  // already uses a monotonic relative timeout and we don't honour
  // absolute REALTIME deadlines. Strip it rather than reject so
  // portable callers don't need a Windows-specific code path.
  static constexpr int FUTEX_CLOCK_REALTIME = 256;

  int op = futex_op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

  switch (op) {
  case FUTEX_WAIT: {
    if (timeout &&
        (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L ||
         timeout->tv_sec < 0))
      return Error(EINVAL);
    // Classical FUTEX_WAIT is FUTEX_WAIT_BITSET with bitset = ~0u.
    intptr_t ret = futex_addr::wait(uaddr, val, timeout, 0xFFFFFFFFu);
    if (ret == 0)
      return 0L;
    if (ret == -EAGAIN)
      return Error(EAGAIN);
    if (ret == -ETIMEDOUT)
      return Error(ETIMEDOUT);
    return Error(EINVAL);
  }
  case FUTEX_WAKE:
    // Classical FUTEX_WAKE is FUTEX_WAKE_BITSET with wake_mask = ~0u.
    return futex_addr::wake(uaddr, val, 0xFFFFFFFFu);
  case FUTEX_WAIT_BITSET: {
    // Linux rejects bitset==0 rather than block forever.
    if (val3 == 0)
      return Error(EINVAL);
    if (timeout &&
        (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L ||
         timeout->tv_sec < 0))
      return Error(EINVAL);
    intptr_t ret = futex_addr::wait(uaddr, val, timeout, val3);
    if (ret == 0)
      return 0L;
    if (ret == -EAGAIN)
      return Error(EAGAIN);
    if (ret == -ETIMEDOUT)
      return Error(ETIMEDOUT);
    return Error(EINVAL);
  }
  case FUTEX_WAKE_BITSET:
    // Linux rejects bitset==0; an all-zero mask would wake nobody.
    if (val3 == 0)
      return Error(EINVAL);
    return futex_addr::wake(uaddr, val, val3);
  default:
    return Error(ENOSYS);
  }
}

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_FUTEX_H
