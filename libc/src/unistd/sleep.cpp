//===-- Implementation of sleep --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/sleep.h"
#include "hdr/types/struct_timespec.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/time/nanosleep.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(unsigned, sleep, (unsigned seconds)) {
  struct timespec req = {static_cast<time_t>(seconds), 0};
  struct timespec rem = {0, 0};
  if (LIBC_NAMESPACE::nanosleep(&req, &rem) == 0)
    return 0;
  // Interrupted by signal — return remaining seconds, rounded up.
  unsigned remaining = static_cast<unsigned>(rem.tv_sec);
  if (rem.tv_nsec > 0)
    ++remaining;
  return remaining;
}

} // namespace LIBC_NAMESPACE_DECL
