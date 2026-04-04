//===-- Implementation of pthread_mutexattr_getkind_np --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_mutexattr_getkind_np.h"
#include "pthread_mutexattr.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// LinuxThreads-era spelling of pthread_mutexattr_gettype. Glibc keeps
// it as a strict alias, so its return values and storage layout match
// gettype exactly. Implemented inline rather than calling gettype so
// we don't pull in a second symbol resolution at link time.
LLVM_LIBC_FUNCTION(int, pthread_mutexattr_getkind_np,
                   (const pthread_mutexattr_t *__restrict attr,
                    int *__restrict kind)) {
  *kind = (*attr & unsigned(PThreadMutexAttrPos::TYPE_MASK)) >>
          unsigned(PThreadMutexAttrPos::TYPE_SHIFT);
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
