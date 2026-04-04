//===-- Windows implementation of __sched_setcpuzero ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sched/sched_setcpuzero.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/null_check.h"

#include "hdr/types/cpu_set_t.h"
#include "hdr/types/size_t.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, __sched_setcpuzero,
                   (const size_t cpuset_size, cpu_set_t *set)) {
  LIBC_CRASH_ON_NULLPTR(set);
  __builtin_memset(set, 0, cpuset_size);
}

} // namespace LIBC_NAMESPACE_DECL
