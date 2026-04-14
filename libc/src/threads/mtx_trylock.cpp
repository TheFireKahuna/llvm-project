//===-- Implementation of mtx_trylock -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/threads/mtx_trylock.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/mutex.h"

#include <threads.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, mtx_trylock, (mtx_t * mutex)) {
  auto err = reinterpret_cast<Mutex *>(mutex)->try_lock();
  if (err == MutexError::NONE)
    return thrd_success;
  if (err == MutexError::BUSY)
    return thrd_busy;
  return thrd_error;
}

} // namespace LIBC_NAMESPACE_DECL
