//===-- Implementation of usleep -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/usleep.h"
#include "hdr/types/struct_timespec.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/time/nanosleep.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, usleep, (useconds_t usec)) {
  struct timespec req = {
      static_cast<time_t>(usec / 1000000u),
      static_cast<long>(usec % 1000000u) * 1000L,
  };
  return LIBC_NAMESPACE::nanosleep(&req, nullptr);
}

} // namespace LIBC_NAMESPACE_DECL
