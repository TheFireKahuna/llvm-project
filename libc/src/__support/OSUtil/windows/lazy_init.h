//===-- Single-flight CAS-gated lazy subsystem init -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// LazyInit<F> — thread-safe first-use gate for Tier B subsystems that
// don't need to initialize during __libc_dll_init(). The first ensure()
// call runs F; subsequent calls early-return after an acquire load.
//
// Pattern:
//
//   namespace {
//   int epoll_startup_init_impl() { ... }
//   LazyInit<&epoll_startup_init_impl> g_epoll_init;
//   } // namespace
//
//   long epoll_create1(int flags) {
//     g_epoll_init.ensure();
//     // ... real epoll_create1 body ...
//   }
//
// Invariants:
//   - InitFn is called exactly once across the process. Losers spin-wait.
//   - InitFn returns 0 on success, non-zero on failure; a failure aborts
//     the process (STATUS_DLL_INIT_FAILED). Alternative handling policies
//     push complexity to every call site (return error) or mask bugs
//     (retry on next ensure). Abort matches __libc_bootstrap's failure
//     behavior and keeps call sites clean.
//   - Callable only after Tier B completion (pcb_init_state() == TierB).
//     Debug builds assert; release builds trust the invariant.
//   - Tier A precondition: ensure_slow runs InitFn under a FaultGuard,
//     which depends on (a) the master VEH being installed and (b) the
//     fault_guard_tls_index being allocated. Both happen during Tier A
//     Phase 0b/0d. Any LazyInit fired before Phase 0d completes would
//     attempt fault_guard_enter against a zero TLS index and either
//     reach a missing master handler or push a guard frame the master
//     cannot find — neither is recoverable. The TierB precondition
//     above is strictly stronger (TierB > TierA_FiltersInstalled), so
//     the existing assert covers this case as long as no Tier A code
//     reaches a LazyInit call site.
//
// Fork semantics:
//   - state_ is COW-inherited from parent. If a subsystem was initialized
//     pre-fork, the child sees state_ == kReady but its internal state
//     may be invalid (stale kernel handles, reactor tokens, …).
//   - Subsystems that need fork repair call reset_for_fork() from their
//     xxx_fork_reinit() hook; the next ensure() rebuilds state.
//   - Subsystems that were never initialized in the parent stay
//     uninitialized in the child — no reinit cost.
//   - was_initialized() answers "did the parent ever touch this?" for
//     fork repair logic that needs to short-circuit.
//
// Memory model:
//   - Winning CAS uses ACQUIRE (so InitFn sees any prior state).
//   - Final store uses RELEASE (so losers see init-produced state).
//   - was_initialized() loads with ACQUIRE.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LAZY_INIT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LAZY_INIT_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

template <int (*InitFn)()>
class LazyInit {
public:
  // Fast path: single acquire load. If ready, return without touching
  // the slow path.
  LIBC_INLINE void ensure() {
    uint32_t state = state_.load(cpp::MemoryOrder::ACQUIRE);
    if (state == kReady)
      return;
    ensure_slow();
  }

  // True if InitFn has already run (successfully) at least once since
  // the last reset_for_fork(). Used by fork-reinit hooks to skip
  // subsystems that the parent never touched.
  //
  // Not marked const because cpp::Atomic<T>::load is non-const in this
  // libc's atomic implementation.
  LIBC_INLINE bool was_initialized() {
    return state_.load(cpp::MemoryOrder::ACQUIRE) == kReady;
  }

  // Mark the subsystem uninitialized so the next ensure() re-runs InitFn.
  // Two callers:
  //   - Fork-reinit hooks, after subsystem-specific repair (tears down
  //     stale handles / kernel state, then clears the gate).
  //   - The `.libclzr` walker during exec_self_hollow(), which
  //     clears the gate for every registered LazyInit<> in one sweep so
  //     the new image re-runs InitFn on first use.
  //
  // Both callers run single-threaded (post-fork child / exec-quiesced
  // process), so observing kInProgress here means a thread escaped
  // quiescence — corrupt state. Trip the assert; release builds CAS
  // from kReady so a stray kInProgress doesn't get clobbered into
  // never-finishing.
  LIBC_INLINE void reset() {
    uint32_t prev = state_.load(cpp::MemoryOrder::ACQUIRE);
    LIBC_ASSERT(prev != kInProgress &&
                "LazyInit::reset called while InitFn in progress");
    if (prev == kReady) {
      uint32_t expected = kReady;
      state_.compare_exchange_strong(expected, kUninitialized,
                                     cpp::MemoryOrder::RELEASE,
                                     cpp::MemoryOrder::RELAXED);
    }
  }

  // Intent-naming synonym used at fork-reinit callsites for readability.
  LIBC_INLINE void reset_for_fork() { reset(); }

private:
  static constexpr uint32_t kUninitialized = 0;
  static constexpr uint32_t kInProgress = 1;
  static constexpr uint32_t kReady = 2;

  // STATUS_DLL_INIT_FAILED — used when InitFn reports failure.
  static constexpr uint32_t kInitFailedStatus = 0xC0000142u;

  // Out-of-line to keep the ensure() fast path small and predictable.
  [[gnu::noinline]] void ensure_slow();

  cpp::Atomic<uint32_t> state_{kUninitialized};
};

template <int (*InitFn)()>
void LazyInit<InitFn>::ensure_slow() {
  // Precondition — designed for subsystems that come up after
  // __libc_dll_init() has completed. If this fires, either something
  // called lazy-init code during Tier B bring-up (architectural bug),
  // or the call is happening before Tier B has started (entry-point
  // ordering bug).
  LIBC_ASSERT(pcb_init_state() == PcbInitState::TierB &&
              "LazyInit::ensure called before Tier B complete");

  uint32_t expected = kUninitialized;
  if (state_.compare_exchange_strong(expected, kInProgress,
                                     cpp::MemoryOrder::ACQUIRE,
                                     cpp::MemoryOrder::ACQUIRE)) {
    // Winner — run the subsystem init under a FaultGuard so an InitFn
    // that throws or faults aborts cleanly instead of leaving state_
    // permanently kInProgress (which would deadlock every subsequent
    // ensure() spin-waiter). Mask covers all hardware faults except
    // stack overflow / debugger traps (FAULT_GUARD_DTOR semantics).
    int rc;
    {
      windows::FaultGuard g;
      if (windows::fault_guard_enter(&g, windows::FAULT_GUARD_DTOR)) {
        // InitFn faulted — treat as init failure.
        ::NtTerminateProcess(NtCurrentProcess(),
                             static_cast<NTSTATUS>(kInitFailedStatus));
        __builtin_unreachable();
      }
      rc = InitFn();
      windows::fault_guard_leave(&g);
    }
    if (rc != 0) {
      ::NtTerminateProcess(NtCurrentProcess(),
                           static_cast<NTSTATUS>(kInitFailedStatus));
      __builtin_unreachable();
    }
    state_.store(kReady, cpp::MemoryOrder::RELEASE);
    return;
  }

  // Loser — either another thread is currently running InitFn, or it's
  // already finished. Spin-wait until ready. InitFn bodies are short
  // (microseconds), so a pause-spin is cheaper than a kernel wait.
  while (state_.load(cpp::MemoryOrder::ACQUIRE) != kReady) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LAZY_INIT_H
