//===-- Linux implementation of the mtx_init function ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/threads/mtx_init.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#include <threads.h> // For mtx_t definition.

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(Mutex) <= sizeof(mtx_t),
              "The public mtx_t type cannot accommodate the internal mutex "
              "type.");

LLVM_LIBC_FUNCTION(int, mtx_init, (mtx_t * m, int type)) {
  // Convert C11 mtx_recursive flag to POSIX mutex type.
  int mutex_type = (type & mtx_recursive) ? PTHREAD_MUTEX_RECURSIVE
                                          : PTHREAD_MUTEX_NORMAL;
  auto err = Mutex::init(reinterpret_cast<Mutex *>(m), type & mtx_timed,
                         mutex_type, /* is_robust */ false,
                         /* is_pshared */ false);
  return err == MutexError::NONE ? thrd_success : thrd_error;
}

} // namespace LIBC_NAMESPACE_DECL
