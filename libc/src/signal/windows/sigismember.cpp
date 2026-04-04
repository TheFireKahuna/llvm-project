//===-- Windows implementation of sigismember ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigismember.h"
#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/sigset_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sigismember, (const sigset_t *set, int signum)) {
  if (!set || signum <= 0 || signum > NSIG ||
      (signum >= __SIGRTMIN && signum < SIGRTMIN)) {
    libc_errno = EINVAL;
    return -1;
  }
  int word = (signum - 1) / 32;
  int bit = (signum - 1) % 32;
  return (set->__signals[word] >> bit) & 1;
}

} // namespace LIBC_NAMESPACE_DECL
