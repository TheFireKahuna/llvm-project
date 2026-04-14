//===-- Sense-reversing barrier implementation -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/barrier.h"
#include "hdr/errno_macros.h"
#include "hdr/pthread_macros.h"

namespace LIBC_NAMESPACE_DECL {

int Barrier::init(Barrier *b, const pthread_barrierattr_t *attr,
                  unsigned count) {
  // Cross-process barriers require shared futex, which is not supported.
  if (attr != nullptr && attr->pshared != PTHREAD_PROCESS_PRIVATE)
    return ENOTSUP;
  if (count == 0)
    return EINVAL;
  b->expected = count;
  b->sense.init(0);
  b->count.set(count);
  return 0;
}

int Barrier::wait() {
  // Snapshot the current generation. RELAXED is safe: same-thread coherence
  // guarantees this returns the value observed by our last ACQUIRE load
  // (or the init value for the first call, visible via external sync).
  FutexValueType local_sense = sense.load(cpp::MemoryOrder::RELAXED);

  // Arrive. ACQ_REL forms the synchronization chain:
  //   RELEASE -- publishes this thread's pre-barrier stores.
  //   ACQUIRE -- sees all previously-arrived threads' stores (transitively,
  //              via the modification order on count).
  unsigned old = count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);

  if (old == 1) {
    // Last thread: reset count for next generation, then flip sense.
    count.store(expected, cpp::MemoryOrder::RELAXED);
    // RELEASE pairs with ACQUIRE in the wait loop below, completing
    // the full barrier: waiting threads see everything we saw.
    sense.store(local_sense ^ 1, cpp::MemoryOrder::RELEASE);
    sense.notify_all();
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }

  // Park until the generation advances. The futex handles compare-and-wait
  // atomicity: if sense changed between our load and the wait call, wait
  // returns immediately without blocking.
  while (sense.load(cpp::MemoryOrder::ACQUIRE) == local_sense)
    sense.wait(local_sense);
  return 0;
}

int Barrier::destroy(Barrier *) { return 0; }

} // namespace LIBC_NAMESPACE_DECL
