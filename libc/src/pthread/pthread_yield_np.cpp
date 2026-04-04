//===-- Implementation of pthread_yield_np --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_yield_np.h"

#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// BSD-style spelling of pthread_yield. Same semantics: scheduler hint
// to relinquish the remainder of the current quantum. Kept distinct
// from pthread_yield so callers compiled against either spelling can
// link directly without relying on PE forwarders.
LLVM_LIBC_FUNCTION(int, pthread_yield_np, ()) {
  long ret = syscall_impl<long>(SYS_sched_yield);
  return ret == 0 ? 0 : static_cast<int>(-ret);
}

} // namespace LIBC_NAMESPACE_DECL
