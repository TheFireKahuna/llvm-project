//===-- Implementation of pthread_setaffinity_np --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_setaffinity_np.h"

#include "hdr/errno_macros.h"
#include "hdr/types/cpu_set_t.h"
#include "hdr/types/size_t.h"
#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(pthread_t) == sizeof(LIBC_NAMESPACE::Thread),
              "Mismatch between pthread_t and internal Thread.");

// Glibc layout: pthread_setaffinity_np forwards to sched_setaffinity on
// the thread's kernel TID. POSIX-trust on `th` (per pthread_kill
// precedent): if the user passes a stale pthread_t, behavior is
// undefined per POSIX, so we don't pay for cross-thread validation.
LLVM_LIBC_FUNCTION(int, pthread_setaffinity_np,
                   (pthread_t th, size_t cpusetsize,
                    const cpu_set_t *cpuset)) {
  if (!cpuset || cpusetsize == 0)
    return EINVAL;

  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib)
    return ESRCH;

  int tid = attrib->tid;
  if (tid <= 0)
    return ESRCH;

  // Linux's sched_setaffinity returns 0 on success, -errno on failure;
  // syscall_impl returns the same convention.
  long ret = syscall_impl<long>(SYS_sched_setaffinity, tid, cpusetsize,
                                reinterpret_cast<long>(cpuset));
  return ret == 0 ? 0 : static_cast<int>(-ret);
}

} // namespace LIBC_NAMESPACE_DECL
