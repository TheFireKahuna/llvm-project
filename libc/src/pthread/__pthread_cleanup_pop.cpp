//===-- Implementation of __pthread_cleanup_pop ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "__pthread_cleanup_pop.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cancel_support.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, __pthread_cleanup_pop,
                   (__pthread_cleanup_t * frame, int execute)) {
  // Null __routine marks the frame as already popped — the
  // __attribute__((cleanup)) noop checks this to avoid a redundant call.
  auto routine = frame->__routine;
  frame->__routine = nullptr;

  cancel::set_cleanup_head(frame->__prev);

  if (execute && routine)
    routine(frame->__arg);
}

} // namespace LIBC_NAMESPACE_DECL
