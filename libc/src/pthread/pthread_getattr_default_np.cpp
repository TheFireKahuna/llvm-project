//===-- Implementation of pthread_getattr_default_np ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_getattr_default_np.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/default_attr.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

// Returns a zero-initialized pthread_attr_t with stacksize/guardsize
// populated from the process-wide defaults. Matches musl's restriction:
// only the two fields exposed via pthread_setattr_default_np are
// reported back; everything else is left at zero so callers don't read
// stale per-thread defaults out of a process-wide accessor.
LLVM_LIBC_FUNCTION(int, pthread_getattr_default_np, (pthread_attr_t * attr)) {
  if (attr == nullptr)
    return EINVAL;

  *attr = pthread_attr_t{};
  attr->__stacksize = internal::get_default_stacksize();
  attr->__guardsize = internal::get_default_guardsize();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
