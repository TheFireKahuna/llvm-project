//===-- Implementation of pthread_setattr_default_np ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_setattr_default_np.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/default_attr.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

// Musl-restricted semantics: only stacksize and guardsize are honored.
// Anything else set in `attr` (detachstate, scheduling fields, ...) makes
// the call return EINVAL — the rationale being that those fields are
// per-call decisions, not process-wide defaults. Updates are max-only:
// a smaller request never lowers the stored value.
//
// Implementation note: musl uses memcmp against a zero-init pthread_attr_t
// to catch any non-default field, which is concise but technically depends
// on padding bytes being zero in both operands. Direct field comparison
// sidesteps the padding question and is just as compact for this struct.
// PTHREAD_CREATE_JOINABLE / SCHED_OTHER / PTHREAD_INHERIT_SCHED are all 0,
// so "field == 0" means "unchanged from pthread_attr_init's defaults."
LLVM_LIBC_FUNCTION(int, pthread_setattr_default_np,
                   (const pthread_attr_t *attr)) {
  if (attr == nullptr)
    return EINVAL;

  if (attr->__detachstate != 0 || attr->__stack != nullptr ||
      attr->__schedpolicy != 0 || attr->__inheritsched != 0 ||
      attr->__schedparam.sched_priority != 0)
    return EINVAL;

  internal::bump_default_stacksize(attr->__stacksize);
  internal::bump_default_guardsize(attr->__guardsize);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
