//===-- Internal setitimer/getitimer declarations ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_ITIMER_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_ITIMER_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

struct itimerval;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t setitimer(int which, const struct itimerval *new_value,
               struct itimerval *old_value);
intptr_t getitimer(int which, struct itimerval *curr_value);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TIME_ITIMER_OPS_H
