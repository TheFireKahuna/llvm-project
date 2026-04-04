//===-- Internal clock_getres/clock_getcpuclockid declarations ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: clock functions that implement Linux
// syscall semantics (return 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_CLOCK_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_CLOCK_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/clockid_t.h"
#include "hdr/types/pid_t.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

struct timespec;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Private clockid_t encoding used by pthread_getcpuclockid for arbitrary
// threads. Public clock constants are small positive integers, so the high bit
// cleanly distinguishes an encoded thread CPU clock from the POSIX ids.
inline constexpr uint32_t THREAD_CPUCLOCK_ID_FLAG = 0x80000000u;

LIBC_INLINE bool is_thread_cpuclockid(::clockid_t id) {
  return (static_cast<uint32_t>(id) & THREAD_CPUCLOCK_ID_FLAG) != 0;
}

LIBC_INLINE ::clockid_t make_thread_cpuclockid(uint32_t tid) {
  return static_cast<::clockid_t>((tid & ~THREAD_CPUCLOCK_ID_FLAG) |
                                  THREAD_CPUCLOCK_ID_FLAG);
}

LIBC_INLINE uint32_t thread_cpuclockid_tid(::clockid_t id) {
  return static_cast<uint32_t>(id) & ~THREAD_CPUCLOCK_ID_FLAG;
}

intptr_t clock_getres(::clockid_t id, struct timespec *res);
intptr_t clock_getcpuclockid(pid_t pid, ::clockid_t *clock_id);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_CLOCK_OPS_H
