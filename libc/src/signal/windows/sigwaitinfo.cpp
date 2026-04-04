//===-- Windows implementation of sigwaitinfo ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigwaitinfo.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/signal/sigtimedwait.h"
#include "src/pthread/cancel_internal.h"

namespace LIBC_NAMESPACE_DECL {

// sigwaitinfo is sigtimedwait with no timeout (blocks indefinitely).
LLVM_LIBC_FUNCTION(int, sigwaitinfo,
                   (const sigset_t *__restrict set,
                    siginfo_t *__restrict info)) {
  cancel_check(); // POSIX cancellation point.
  return LIBC_NAMESPACE::sigtimedwait(set, info, nullptr);
}

} // namespace LIBC_NAMESPACE_DECL
