//===-- Windows implementation of sigfillset ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigfillset.h"
#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/sigset_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, sigfillset, (sigset_t * set)) {
  if (!set) {
    libc_errno = EINVAL;
    return -1;
  }
  // Valid signals are 1..NSIG, stored in bits 0..(NSIG-1).
  // NSIG=64: all 64 signal bits set across both words.
  // Exclude reserved internal RT signals __SIGRTMIN..SIGRTMIN-1 (32-33).
  uint64_t all = (NSIG == 64) ? ~uint64_t{0} : ((1ULL << (NSIG - 1)) - 1);
  for (int sig = __SIGRTMIN; sig < SIGRTMIN; ++sig)
    all &= ~(1ULL << (sig - 1));
  set->__signals[0] = static_cast<unsigned long>(all);
  set->__signals[1] = static_cast<unsigned long>(all >> 32);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
