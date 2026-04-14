//===-- Implementation of __pthread_cleanup_push --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "__pthread_cleanup_push.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cancel_support.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, __pthread_cleanup_push,
                   (__pthread_cleanup_t * frame, void (*routine)(void *),
                    void *arg)) {
  frame->__routine = routine;
  frame->__arg = arg;
  frame->__prev = cancel::get_cleanup_head();
  cancel::set_cleanup_head(frame);
}

} // namespace LIBC_NAMESPACE_DECL
