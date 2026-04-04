//===-- Implementation of pthread_sigmask ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_sigmask.h"

#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/signal/sigprocmask.h"

namespace LIBC_NAMESPACE_DECL {

// pthread_sigmask has the same semantics as sigprocmask but returns the
// error number directly instead of returning -1 and setting errno.
LLVM_LIBC_FUNCTION(int, pthread_sigmask,
                   (int how, const sigset_t *__restrict set,
                    sigset_t *__restrict oldset)) {
  int ret = LIBC_NAMESPACE::sigprocmask(how, set, oldset);
  if (ret < 0) {
    // sigprocmask sets errno; pthread_sigmask returns it directly.
    int err = libc_errno;
    libc_errno = 0;
    return err;
  }
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
