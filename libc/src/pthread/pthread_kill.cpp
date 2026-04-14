//===-- Implementation of the pthread_kill function -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_kill.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

LLVM_LIBC_FUNCTION(int, pthread_kill, (pthread_t th, int sig)) {
  if (sig < 0 || sig >= NSIG)
    return EINVAL;

  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib)
    return ESRCH;

  int tid = attrib->tid;
  if (tid <= 0)
    return ESRCH;

  if (sig == 0) {
    // Signal 0 is used to check thread existence.
    long ret = syscall_impl<long>(SYS_tgkill, syscall_impl<long>(SYS_getpid),
                                  tid, 0);
    return ret == 0 ? 0 : ESRCH;
  }
  long ret = syscall_impl<long>(SYS_tgkill, syscall_impl<long>(SYS_getpid),
                                tid, sig);
  return ret == 0 ? 0 : static_cast<int>(-ret);
}

} // namespace LIBC_NAMESPACE_DECL
