//===-- Linux implementation of waitid ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/wait/waitid.h"

#include "src/__support/OSUtil/syscall.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include <sys/syscall.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, waitid,
                   (idtype_t idtype, id_t id, siginfo_t *infop, int options)) {
#ifdef SYS_waitid
  long ret = LIBC_NAMESPACE::syscall_impl<long>(SYS_waitid, idtype, id, infop,
                                                options, nullptr);
  if (ret < 0) {
    libc_errno = -ret;
    return -1;
  }
  return 0;
#else
#error "waitid syscall not available."
#endif
}

} // namespace LIBC_NAMESPACE_DECL
