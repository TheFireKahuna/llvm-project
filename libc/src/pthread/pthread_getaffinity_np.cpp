//===-- Implementation of pthread_getaffinity_np --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_getaffinity_np.h"

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

LLVM_LIBC_FUNCTION(int, pthread_getaffinity_np,
                   (pthread_t th, size_t cpusetsize, cpu_set_t *cpuset)) {
  if (!cpuset || cpusetsize == 0)
    return EINVAL;

  auto *thread = reinterpret_cast<Thread *>(&th);
  auto *attrib = thread->attrib;
  if (!attrib)
    return ESRCH;

  int tid = attrib->tid;
  if (tid <= 0)
    return ESRCH;

  // Linux's sched_getaffinity returns the number of bytes written on
  // success, -errno on failure. Glibc's pthread_getaffinity_np returns
  // 0 on success — discard the byte count.
  long ret = syscall_impl<long>(SYS_sched_getaffinity, tid, cpusetsize,
                                reinterpret_cast<long>(cpuset));
  if (ret < 0)
    return static_cast<int>(-ret);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
