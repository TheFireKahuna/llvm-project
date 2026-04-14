//===-- Implementation of the pthread_cond_broadcast function --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_cond_broadcast.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/CndVar.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

static_assert(sizeof(CndVar) <= sizeof(pthread_cond_t),
              "The public pthread_cond_t type cannot accommodate the internal "
              "CndVar type.");

LLVM_LIBC_FUNCTION(int, pthread_cond_broadcast, (pthread_cond_t * cond)) {
  CndVar *cndvar = reinterpret_cast<CndVar *>(cond);
  cndvar->broadcast();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
