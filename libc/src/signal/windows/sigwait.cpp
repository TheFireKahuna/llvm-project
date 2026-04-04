//===-- Windows implementation of sigwait ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigwait.h"
#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/signal/sigtimedwait.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sigwait,
                   (const sigset_t *__restrict set, int *__restrict sig)) {
  cancel_check(); // POSIX cancellation point.
  if (!set || !sig)
    return EINVAL;

  // sigwait blocks indefinitely — pass nullptr timeout to sigtimedwait.
  int result = LIBC_NAMESPACE::sigtimedwait(set, nullptr, nullptr);
  if (result > 0) {
    *sig = result;
    return 0;
  }

  // sigtimedwait returns -1 on error and sets errno.
  // sigwait returns the errno directly (no -1 convention).
  return libc_errno;
}

} // namespace LIBC_NAMESPACE_DECL
