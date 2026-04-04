//===-- CAS-guarded idempotent init latch ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 3-state one-shot initialization latch. Winner runs init; losers wait.
//
//   UNINIT (0)  --CAS win--> INITIALIZING (1) --release store--> READY (2)
//
// The init body is expected to be infallible (trap on any failure it can't
// handle). There is deliberately no failure edge back to UNINIT: every
// failure mode in the real callers (page_reserve/page_commit/ProcessPrng)
// indicates a process-global condition (OOM, commit-limit, entropy source
// gone) that retry cannot recover. Trapping at the point of failure gives
// a useful crash dump; resetting UNINIT would just re-trap on the next
// thread. If a new caller genuinely needs retry, build that at the
// callsite — don't smuggle it back into the latch.
//
// Publication ordering:
//   - winner's init writes happen-before publish_ready()'s RELEASE store.
//   - is_ready() / wait_ready() use ACQUIRE loads.
//   - try_begin() is ACQ_REL on success / ACQUIRE on failure.
//
// wait_ready() uses spin_wait::spin_on_slot_state (hardware UMWAIT/MWAITX
// when available, RELAXED-load fallback otherwise).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_INIT_LATCH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_INIT_LATCH_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/spin_wait.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace alloc_primitives {

inline constexpr uint8_t INIT_UNINIT = 0;
inline constexpr uint8_t INIT_INITIALIZING = 1;
inline constexpr uint8_t INIT_READY = 2;

class InitLatch {
  // `mutable` so is_ready() can be const — mirrors std::atomic convention.
  mutable cpp::Atomic<uint8_t> state_{INIT_UNINIT};

public:
  LIBC_INLINE constexpr InitLatch() = default;
  InitLatch(const InitLatch &) = delete;
  InitLatch &operator=(const InitLatch &) = delete;

  // Try to become the initializer. Returns true on UNINIT -> INITIALIZING
  // CAS success. After a winning try_begin(), the caller must either call
  // publish_ready() or trap — there is no failure edge.
  [[nodiscard]] LIBC_INLINE bool try_begin() {
    uint8_t expected = INIT_UNINIT;
    return state_.compare_exchange_strong(expected, INIT_INITIALIZING,
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::ACQUIRE);
  }

  // Publish READY after successful init. Release store. Call exactly once
  // after a winning try_begin(). The RELEASE paired with ACQUIRE loads
  // elsewhere establishes happens-before for the winner's init writes.
  LIBC_INLINE void publish_ready() {
    state_.store(INIT_READY, cpp::MemoryOrder::RELEASE);
  }

  // Block (hardware-monitor spin) until READY is observed.
  LIBC_INLINE void wait_ready() const {
    for (;;) {
      uint8_t s = state_.load(cpp::MemoryOrder::ACQUIRE);
      if (s == INIT_READY)
        return;
      spin_wait::spin_on_slot_state(&state_, s);
    }
  }

  // Non-blocking check. Acquire load.
  [[nodiscard]] LIBC_INLINE bool is_ready() const {
    return state_.load(cpp::MemoryOrder::ACQUIRE) == INIT_READY;
  }

  // Post-fork repair. Legal only on the forking thread in the child before
  // other threads are recreated. If a dead (non-forking) thread was mid-
  // init at fork time the state word is stuck at INITIALIZING — restore
  // UNINIT so the surviving thread can re-initialize. READY and UNINIT
  // are left untouched.
  LIBC_INLINE void fork_reinit() {
    if (state_.load(cpp::MemoryOrder::RELAXED) == INIT_INITIALIZING)
      state_.store(INIT_UNINIT, cpp::MemoryOrder::RELAXED);
  }
};

static_assert(sizeof(InitLatch) == 1, "InitLatch must be exactly 1 byte");

} // namespace alloc_primitives
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_INIT_LATCH_H
