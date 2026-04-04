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
    // store_and_notify_all (SEQ_CST store + notify_all) — NOT
    // store(RELEASE)+notify_all. On x86 the split form reorders the
    // MOV to value_ past the MOV from stack_ in notify_all, so a peer
    // that pushed itself onto stack_ right after reading the old sense
    // can be stranded. store_and_notify_all uses SEQ_CST (LOCK-prefixed
    // or MFENCE) which drains the store buffer before the notify's load.
    sense.store_and_notify_all(local_sense ^ 1);
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }

  // Park until the generation advances. The futex handles compare-and-wait
  // atomicity: if sense changed between our load and the wait call, wait
  // returns immediately without blocking.
  //
  // Fatal-error exit: if the wait cannot park (e.g. -ENOMEM on wait-slot
  // pool exhaustion), break out rather than spin-calling wait in a hot
  // loop. The sense value may not have advanced yet — in that rare case
  // the caller sees a barrier that "ended early" for this participant.
  // That matches POSIX semantics for pthread_barrier_wait under errno
  // conditions and is strictly better than burning a core. EINTR is
  // absorbed internally by Futex::wait<false>.
  while (sense.load(cpp::MemoryOrder::ACQUIRE) == local_sense) {
    long ret = sense.wait(local_sense);
    if (ret < 0 && ret != -EINTR)
      break;
  }
  return 0;
}

int Barrier::destroy(Barrier *) { return 0; }

} // namespace LIBC_NAMESPACE_DECL
