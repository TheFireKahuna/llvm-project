//===-- Implementation of pthread_setcanceltype ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_setcanceltype.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cancel_support.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_setcanceltype, (int type, int *oldtype)) {
  if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
    return EINVAL;

  auto *word = cancel::get_cancel_word();
  if (!word)
    return EINVAL;

  // CAS loop to flip bit 1 (TYPE_BIT) in the packed cancel_state.
  uint8_t old_word = word->load(cpp::MemoryOrder::RELAXED);
  uint8_t new_word;
  do {
    new_word = type == PTHREAD_CANCEL_ASYNCHRONOUS
                   ? (old_word | cancel::TYPE_BIT)
                   : (old_word & ~cancel::TYPE_BIT);
  } while (!word->compare_exchange_weak(old_word, new_word,
                                        cpp::MemoryOrder::RELAXED,
                                        cpp::MemoryOrder::RELAXED));

  if (oldtype)
    *oldtype = (old_word & cancel::TYPE_BIT) ? PTHREAD_CANCEL_ASYNCHRONOUS
                                             : PTHREAD_CANCEL_DEFERRED;

  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
