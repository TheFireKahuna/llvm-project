//===--- nanosleep internal interface ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_TIME_NANOSLEEP_H
#define LLVM_LIBC_SRC___SUPPORT_TIME_NANOSLEEP_H

#include "hdr/types/struct_timespec.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Kernel-convention nanosleep: returns 0 on success, -errno on failure.
// On signal interruption, writes remaining time to |rem| and returns -EINTR.
long nanosleep(const timespec *req, timespec *rem);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_TIME_NANOSLEEP_H
