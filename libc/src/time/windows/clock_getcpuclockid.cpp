//===-- Windows implementation of clock_getcpuclockid ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/clock_getcpuclockid.h"
#include "src/__support/OSUtil/windows/time/clock_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, clock_getcpuclockid,
                   (pid_t pid, clockid_t *clock_id)) {
  intptr_t ret = internal::clock_getcpuclockid(pid, clock_id);
  if (ret < 0)
    return static_cast<int>(-ret);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
