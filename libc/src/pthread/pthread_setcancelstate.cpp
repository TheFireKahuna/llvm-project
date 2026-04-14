//===-- Implementation of pthread_setcancelstate --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_setcancelstate.h"

#include "hdr/errno_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cancel_support.h"

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_setcancelstate, (int state, int *oldstate)) {
  if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
    return EINVAL;

  auto *word = cancel::get_cancel_word();
  if (!word)
    return EINVAL;

  // CAS loop to flip bit 0 (STATE_BIT) in the packed cancel_word.
  uint8_t old_word = word->load(cpp::MemoryOrder::RELAXED);
  uint8_t new_word;
  do {
    new_word = state == PTHREAD_CANCEL_DISABLE
                   ? (old_word | cancel::STATE_BIT)
                   : (old_word & ~cancel::STATE_BIT);
  } while (!word->compare_exchange_weak(old_word, new_word,
                                        cpp::MemoryOrder::RELAXED,
                                        cpp::MemoryOrder::RELAXED));

  if (oldstate)
    *oldstate = (old_word & cancel::STATE_BIT) ? PTHREAD_CANCEL_DISABLE
                                               : PTHREAD_CANCEL_ENABLE;

  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
