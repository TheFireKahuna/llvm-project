//===-- Implementation of pthread_mutexattr_setkind_np --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_mutexattr_setkind_np.h"
#include "pthread_mutexattr.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <errno.h>
#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

// LinuxThreads-era spelling of pthread_mutexattr_settype. Same accepted
// kind values, same EINVAL on anything else. Glibc additionally
// accepts a fourth value (PTHREAD_MUTEX_FAST_NP, an alias for NORMAL)
// in older releases — we follow the modern convention and reject it,
// matching pthread_mutexattr_settype's validation set.
LLVM_LIBC_FUNCTION(int, pthread_mutexattr_setkind_np,
                   (pthread_mutexattr_t *__restrict attr, int kind)) {
  if (kind != PTHREAD_MUTEX_NORMAL && kind != PTHREAD_MUTEX_ERRORCHECK &&
      kind != PTHREAD_MUTEX_RECURSIVE) {
    return EINVAL;
  }
  pthread_mutexattr_t old = *attr;
  old &= ~unsigned(PThreadMutexAttrPos::TYPE_MASK);
  *attr = old | (kind << unsigned(PThreadMutexAttrPos::TYPE_SHIFT));
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
