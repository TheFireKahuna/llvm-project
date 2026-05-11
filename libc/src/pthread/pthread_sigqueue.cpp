//===-- Implementation of pthread_sigqueue --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_sigqueue.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/union_sigval.h"
#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

// Glibc's pthread_sigqueue is implemented atop rt_tgsigqueueinfo. We
// build a minimal siginfo (si_signo, si_code=SI_QUEUE, si_value, si_pid,
// si_uid populated by the engine) and forward through the syscall fast
// path so the same machinery that backs sigqueue(3) handles the RT entry
// allocation, payload routing, and APC delivery.
LLVM_LIBC_FUNCTION(int, pthread_sigqueue,
                   (pthread_t th, int sig, const union sigval value)) {
  if (sig < 0 || sig >= NSIG)
    return EINVAL;

  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib)
    return ESRCH;

  int tid = attrib->tid;
  if (tid <= 0)
    return ESRCH;

  // Linux's rt_tgsigqueueinfo wants a (tgid, tid, sig, siginfo). The
  // engine ignores tgid (Windows shares one PID across threads) but we
  // pass the real PID for symmetry with signal_state::sigqueue's
  // intra-process check on other platforms.
  siginfo_t info = {};
  info.si_signo = sig;
  info.si_code = SI_QUEUE;
  info.si_value = value;
  // si_pid / si_uid are filled by the engine on dispatch — leaving them
  // zero here matches glibc's behavior, which does the same.

  long ret = syscall_impl<long>(SYS_rt_tgsigqueueinfo,
                                syscall_impl<long>(SYS_getpid), tid, sig,
                                reinterpret_cast<intptr_t>(&info));
  return ret == 0 ? 0 : static_cast<int>(-ret);
}

} // namespace LIBC_NAMESPACE_DECL
