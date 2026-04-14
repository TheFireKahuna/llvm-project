//===-- Implementation header for futimes ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SYS_TIME_FUTIMES_H
#define LLVM_LIBC_SRC_SYS_TIME_FUTIMES_H

#include "hdr/types/struct_timeval.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int futimes(int fd, const struct timeval times[2]);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SYS_TIME_FUTIMES_H
