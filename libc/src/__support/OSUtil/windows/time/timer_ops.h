//===-- Internal timer operation declarations -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: timer functions that implement POSIX
// timer semantics (return 0 on success, -errno on failure; timer_getoverrun
// returns count >= 0 on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_TIMER_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_TIMER_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"
#include "hdr/types/clockid_t.h"
#include "hdr/types/timer_t.h"

struct sigevent;
struct itimerspec;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t timer_create(clockid_t clockid, struct sigevent *__restrict sevp,
                  timer_t *__restrict timerid);
intptr_t timer_settime(timer_t timerid, int flags,
                   const struct itimerspec *__restrict new_value,
                   struct itimerspec *__restrict old_value);
intptr_t timer_gettime(timer_t timerid, struct itimerspec *curr_value);
intptr_t timer_getoverrun(timer_t timerid);
intptr_t timer_delete(timer_t timerid);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_TIMER_OPS_H
