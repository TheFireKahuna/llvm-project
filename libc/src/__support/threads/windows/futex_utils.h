//===--- Futex for Windows with CAS-64 Treiber wait stack -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 8-byte Futex: union of uint64_t combined_ over [stack_:32 | value_:32]
//
// value_    — 32-bit caller-visible futex value (upper 32 bits on LE).
//             All RMW operations (fetch_add, fetch_or, etc.) are direct
//             single-instruction atomics on this word.
//
// stack_    — 32-bit Treiber wait stack packed as [gen:16 | top:16]:
//             gen = ABA counter, top = slot index into WaitSlot pool.
//             Lock-free push (CAS-64 on combined_) and pop (CAS-32 on
//             stack_). No spinlock anywhere.
//
// combined_ — 64-bit view for atomic value-check + push. A single CAS-64
//             instruction atomically verifies the value hasn't changed AND
//             pushes the waiter onto the stack. This eliminates the Dekker
//             protocol entirely — no lost-wakeup window, no cancel ghosts.
//
// Pop-before-signal: The waker pops a slot from the stack (CAS-32 on
// stack_) BEFORE signaling it. Once popped, the slot is detached — safe
// to signal and recycle. This prevents the stale-next corruption that
// plagues lock-free linked queues without GC.
//
// LIFO wake order: most recently parked thread wakes first. This is
// cache-friendly (warmest data) and matches Linux qspinlock / parking_lot.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_UTILS_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/optional.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/futex_word.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/wait_slot.h"
#include "src/__support/time/abs_timeout.h"

namespace LIBC_NAMESPACE_DECL {

class Futex {
  // Little-endian layout:
  //   offset 0: stack_  (lower 32 bits) — [gen:16 | top:16]
  //   offset 4: value_  (upper 32 bits) — user-visible futex value
  //
  // CAS-64 on combined_ atomically operates on both.
  // Native RMW on value_ (fetch_add, etc.) doesn't touch stack_.
  // CAS-32 on stack_ (pop) doesn't touch value_.
  union {
    cpp::Atomic<uint64_t> combined_;
    struct {
      cpp::Atomic<uint32_t> stack_;
      cpp::Atomic<FutexWordType> value_;
    };
  };

  // Stack word layout: [gen:16 | top:16]
  static constexpr uint32_t STACK_GEN_SHIFT = 16;
  static constexpr uint32_t STACK_INDEX_MASK = 0xFFFFu;
  static constexpr uint16_t STACK_NULL = static_cast<uint16_t>(wait_slot::NULL_INDEX);

  LIBC_INLINE static constexpr uint16_t stack_top(uint32_t s) {
    return static_cast<uint16_t>(s & STACK_INDEX_MASK);
  }
  LIBC_INLINE static constexpr uint16_t stack_gen(uint32_t s) {
    return static_cast<uint16_t>(s >> STACK_GEN_SHIFT);
  }
  LIBC_INLINE static constexpr uint32_t stack_pack(uint16_t gen, uint16_t top) {
    return (static_cast<uint32_t>(gen) << STACK_GEN_SHIFT) | top;
  }

  // Combined word layout: [value:32 (upper) | stack:32 (lower)]
  LIBC_INLINE static constexpr uint32_t combined_value(uint64_t c) {
    return static_cast<uint32_t>(c >> 32);
  }
  LIBC_INLINE static constexpr uint32_t combined_stack(uint64_t c) {
    return static_cast<uint32_t>(c);
  }
  LIBC_INLINE static constexpr uint64_t combined_pack(uint32_t val, uint32_t stk) {
    return (static_cast<uint64_t>(val) << 32) | stk;
  }

public:
  using Timeout = internal::AbsTimeout;

  LIBC_INLINE constexpr Futex(FutexValueType value)
      : combined_(combined_pack(value,
                                stack_pack(0, STACK_NULL))) {}

  // --- Value access ---

  LIBC_INLINE FutexValueType get_value(
      cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) const {
    return const_cast<Futex *>(this)->value_.load(ord);
  }

  // --- Standard atomic operations (direct passthrough to value_) ---

  LIBC_INLINE FutexValueType
  load(cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.load(ord);
  }

  LIBC_INLINE void
  store(FutexValueType val,
        cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    value_.store(val, ord);
  }

  LIBC_INLINE FutexValueType
  exchange(FutexValueType val,
           cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.exchange(val, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_add(FutexValueType delta,
            cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_add(delta, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_sub(FutexValueType delta,
            cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_sub(delta, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_or(FutexValueType mask,
           cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_or(mask, ord);
  }

  LIBC_INLINE FutexValueType
  fetch_and(FutexValueType mask,
            cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return value_.fetch_and(mask, ord);
  }

  LIBC_INLINE bool
  compare_exchange_weak(FutexValueType &expected, FutexValueType desired,
                        cpp::MemoryOrder success_ord,
                        cpp::MemoryOrder failure_ord) {
    return value_.compare_exchange_weak(expected, desired,
                                        success_ord, failure_ord);
  }

  LIBC_INLINE bool
  compare_exchange_weak(FutexValueType &expected, FutexValueType desired,
                        cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return compare_exchange_weak(expected, desired, ord,
                                 ord == cpp::MemoryOrder::RELEASE
                                     ? cpp::MemoryOrder::RELAXED
                                     : ord);
  }

  LIBC_INLINE bool
  compare_exchange_strong(FutexValueType &expected, FutexValueType desired,
                          cpp::MemoryOrder success_ord,
                          cpp::MemoryOrder failure_ord) {
    return value_.compare_exchange_strong(expected, desired,
                                          success_ord, failure_ord);
  }

  LIBC_INLINE bool
  compare_exchange_strong(FutexValueType &expected, FutexValueType desired,
                          cpp::MemoryOrder ord = cpp::MemoryOrder::SEQ_CST) {
    return compare_exchange_strong(expected, desired, ord,
                                   ord == cpp::MemoryOrder::RELEASE
                                       ? cpp::MemoryOrder::RELAXED
                                       : ord);
  }

  // Operator overloads for value comparison.
  LIBC_INLINE operator FutexValueType() {
    return load(cpp::MemoryOrder::SEQ_CST);
  }

  LIBC_INLINE Futex &operator=(FutexValueType value) {
    store(value, cpp::MemoryOrder::RELEASE);
    return *this;
  }

  LIBC_INLINE bool operator==(FutexValueType rhs) {
    return load(cpp::MemoryOrder::SEQ_CST) == rhs;
  }

  LIBC_INLINE bool operator!=(FutexValueType rhs) { return !(*this == rhs); }

  // Full initialization: sets value_ AND clears the Treiber stack to empty.
  // Use for first-time init of opaque storage (pthread_mutex_t, sem_t, etc.)
  // where stack_ may contain garbage from uninitialized memory.  Does NOT
  // drain — there are no legitimate waiters to wake during init.
  LIBC_INLINE void init(FutexValueType val) {
    combined_.store(combined_pack(val, stack_pack(0, STACK_NULL)),
                    cpp::MemoryOrder::RELAXED);
  }

  // Full reset: drains any stale waiters from the embedded Treiber stack,
  // then atomically zeroes both value_ AND stack_. Drain-first ensures
  // stale waiters wake and detect invalidation (wait_address == 0) instead
  // of operating on recycled storage. The drain is a no-op when the stack
  // is empty (one atomic load, branch-not-taken). Always atomic — a
  // non-atomic 64-bit store could tear against a stale waiter's CAS-64.
  LIBC_INLINE void reset(FutexValueType val = 0) {
    drain_waiters();
    combined_.store(combined_pack(val, stack_pack(0, STACK_NULL)),
                    cpp::MemoryOrder::RELEASE);
  }

  // Fork-child reset: atomically zeroes value_ + stack_ WITHOUT draining.
  // After fork, stale waiter slots reference parent-process threads —
  // drain_waiters() would send NtAlertThreadByThreadId to the parent
  // (TIDs are system-wide on Windows), causing spurious wakes there.
  // The child has no threads to wake; just clearing the stack is enough.
  // Stale slot cleanup happens lazily if the child later encounters one
  // via the pop-side wait_address check.
  LIBC_INLINE void reset_for_fork(FutexValueType val = 0) {
    combined_.store(combined_pack(val, stack_pack(0, STACK_NULL)),
                    cpp::MemoryOrder::RELEASE);
  }

  // Drain all waiters from the embedded Treiber stack, signaling each one
  // so it can wake and observe that the futex is no longer valid. Clears
  // wait_address on each evicted slot so waiters can detect invalidation.
  // Used by mutex destroy to prevent stale waiters from operating on
  // recycled storage.
  LIBC_INLINE void drain_waiters() {
    // Steal the entire stack atomically.
    uint32_t old_stk;
    for (;;) {
      old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      if (stack_top(old_stk) == STACK_NULL)
        return;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, STACK_NULL);
      if (stack_.compare_exchange_weak(old_stk, new_stk,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::RELAXED))
        break;
    }

    // Walk the stolen list: clear wait_address (invalidation signal),
    // then signal each slot so the waiter wakes up.
    uint32_t curr = stack_top(old_stk);
    while (curr != wait_slot::NULL_INDEX) {
      auto &slot = wait_slot::get_slot(curr);
      uint32_t next = slot.next;

      // Clear wait_address BEFORE signaling. The waiter checks this
      // after waking to detect that its mutex was destroyed.
      slot.wait_address = 0;

      uint8_t old = slot.state.exchange(wait_slot::SIGNALED,
                                         cpp::MemoryOrder::ACQ_REL);
      if (old == wait_slot::IN_KERNEL) {
        ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(slot.thread_id)));
      } else if (old == wait_slot::TIMED_OUT) {
        // Dead slot — reclaim it.
        slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
        slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
        wait_slot::reclaim_slot(curr);
      }
      curr = next;
    }
  }

  // --- Stack queries ---

  // Lock-free check: any waiters in the embedded stack?
  LIBC_INLINE bool has_waiters() const {
    uint32_t s = const_cast<Futex *>(this)->stack_.load(
        cpp::MemoryOrder::ACQUIRE);
    return stack_top(s) != STACK_NULL;
  }

  // --- Timeout conversion ---

  LIBC_INLINE LARGE_INTEGER *
  timeout_to_nt(cpp::optional<Timeout> timeout, LARGE_INTEGER &storage) {
    if (!timeout)
      return nullptr;

    constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;
    const timespec &ts = timeout->get_timespec();
    long long target_hns =
        static_cast<long long>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100;

    if (timeout->is_realtime()) {
      storage.QuadPart = target_hns + EPOCH_DIFF_HNS;
      return &storage;
    }

    ULONGLONG now_hns;
    ::RtlQueryUnbiasedInterruptTime(&now_hns);
    long long relative_hns = target_hns - static_cast<long long>(now_hns);
    if (relative_hns <= 0) {
      storage.QuadPart = -1;
      return &storage;
    }
    storage.QuadPart = -relative_hns;
    return &storage;
  }

  // ===== Wait (CAS-64 push) =====
  //
  // Atomic value-check + push in a single CAS-64 instruction.
  // If value != expected, the CAS fails (value portion doesn't match)
  // and we return without pushing. No Dekker protocol needed.

  // Interruptible: when true, APC/signal wakes return -EINTR instead
  // of being absorbed as spurious wakes. Zero codegen impact on the
  // default (false) path — the if-constexpr branch is eliminated.
  template <bool Interruptible = false>
  LIBC_INLINE long wait(FutexValueType expected,
                        cpp::optional<Timeout> timeout = cpp::nullopt,
                        bool is_shared = false) {
    (void)is_shared;

    // Fast path: value already changed.
    if (get_value(cpp::MemoryOrder::RELAXED) != expected)
      return 0;

    // Phase 1: hardware spin on the value directly.
    // Always use UMWAIT/MWAITX when available — the hardware monitor
    // wakes on cache-line write with near-zero power (C0.2). This is
    // better than PAUSE spinning at high thread counts (16T+) where
    // PAUSE consumes SMT resources from the lock holder.
    if (spin_wait::spin_until_changed(&value_, expected))
      return 0;

    // Phase 1.5: check for already-expired timeout.
    if (timeout) {
      LARGE_INTEGER probe;
      LARGE_INTEGER *p = timeout_to_nt(timeout, probe);
      if (p && p->QuadPart == -1)
        return -ETIMEDOUT;
    }

    // Phase 2: CAS-64 push onto the Treiber stack.
    uint32_t primary_idx = wait_slot::get_slot_index();
    if (primary_idx == wait_slot::NULL_INDEX)
      return 0;
    bool nested = wait_slot::get_slot(primary_idx).state.load(
                      cpp::MemoryOrder::RELAXED) != wait_slot::IDLE;
    uint32_t my_idx = nested ? wait_slot::alloc_secondary() : primary_idx;
    if (my_idx == wait_slot::NULL_INDEX)
      return 0;

    auto &slot = wait_slot::get_slot(my_idx);
    slot.state.store(wait_slot::WAITING, cpp::MemoryOrder::RELAXED);
    slot.wait_address = reinterpret_cast<uintptr_t>(this);
    slot.handoff = 0;

    auto finish = [nested, my_idx](long ret) -> long {
      if (nested && ret != -ETIMEDOUT && ret != -EINTR &&
          wait_slot::get_slot(my_idx).state.load(
              cpp::MemoryOrder::RELAXED) == wait_slot::IDLE)
        wait_slot::release_secondary(my_idx);
      return ret;
    };

    // CAS-64 push: atomically check value + enqueue.
    //
    // The CAS compares the full 64-bit combined_ word. If value changed
    // (upper 32 bits don't match), the CAS fails and we cancel. If the
    // stack changed (lower 32 bits don't match, concurrent push/pop),
    // the CAS fails and we retry with the updated stack.
    //
    // On success: slot is in the stack AND value was still `expected`.
    // No Dekker re-check needed. The CAS is the linearization point.
    for (;;) {
      uint64_t old = combined_.load(cpp::MemoryOrder::RELAXED);
      uint32_t val = combined_value(old);
      if (val != expected) {
        slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
        slot.wait_address = 0;
        return finish(0);
      }
      uint32_t old_stk = combined_stack(old);
      slot.next = stack_top(old_stk);
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1,
                                     static_cast<uint16_t>(my_idx));
      uint64_t desired = combined_pack(val, new_stk);
      if (combined_.compare_exchange_weak(old, desired,
                                           cpp::MemoryOrder::ACQ_REL,
                                           cpp::MemoryOrder::RELAXED))
        break;
    }

    // Phase 2.5: pre-kernel spin on slot state.
    if (spin_wait::spin_on_slot_state(&slot.state, wait_slot::WAITING)) {
      long ho = slot.handoff;
      slot.handoff = 0;
      bool invalidated = (slot.wait_address == 0);
      slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
      slot.wait_address = 0;
      return finish(invalidated ? -EINVAL : ho);
    }

    // Phase 3: transition to IN_KERNEL.
    {
      uint8_t prev = wait_slot::WAITING;
      if (!slot.state.compare_exchange_strong(prev, wait_slot::IN_KERNEL,
                                              cpp::MemoryOrder::ACQ_REL,
                                              cpp::MemoryOrder::ACQUIRE)) {
        long ho = slot.handoff;
        slot.handoff = 0;
        bool invalidated = (slot.wait_address == 0);
        slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
        slot.wait_address = 0;
        return finish(invalidated ? -EINVAL : ho);
      }
    }

    // Phase 4: kernel sleep.
    LARGE_INTEGER nt_timeout;
    LARGE_INTEGER *nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
    PVOID tid_ptr =
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(slot.thread_id));

    long ret = 0;
    for (;;) {
      uint8_t st = slot.state.load(cpp::MemoryOrder::ACQUIRE);
      if (st != wait_slot::IN_KERNEL) {
        ret = slot.handoff; slot.handoff = 0;
        break;
      }

      NTSTATUS status = ::NtWaitForAlertByThreadId(tid_ptr, nt_timeout_ptr);

      st = slot.state.load(cpp::MemoryOrder::ACQUIRE);

      if (st == wait_slot::SIGNALED) {
        if (status != STATUS_ALERTED) {
          static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
          ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
        }
        ret = slot.handoff; slot.handoff = 0;
        break;
      }

      if (status == STATUS_TIMEOUT) {
        uint8_t prev = wait_slot::IN_KERNEL;
        if (slot.state.compare_exchange_strong(prev, wait_slot::TIMED_OUT,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE)) {
          ret = -ETIMEDOUT;
          break;
        }
        static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
        ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
        ret = slot.handoff; slot.handoff = 0;
        break;
      }

      // Spurious wake (APC / stale alert / kernel noise).
      if constexpr (Interruptible) {
        // Signal APC — self-cancel (CAS IN_KERNEL → TIMED_OUT) and
        // return -EINTR. The waker handles TIMED_OUT slots identically
        // to timeout: exchange→SIGNALED sees TIMED_OUT, reclaims slot.
        uint8_t prev = wait_slot::IN_KERNEL;
        if (slot.state.compare_exchange_strong(prev, wait_slot::TIMED_OUT,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE)) {
          ret = -EINTR;
          break;
        }
        // Waker raced — already set SIGNALED. Drain the pending alert
        // and return the handoff value (normal wake).
        static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
        ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
        ret = slot.handoff; slot.handoff = 0;
        break;
      }

      // Non-interruptible: recompute timeout and retry.
      nt_timeout_ptr = timeout_to_nt(timeout, nt_timeout);
      if (nt_timeout_ptr && nt_timeout_ptr->QuadPart == -1) {
        uint8_t prev = wait_slot::IN_KERNEL;
        if (slot.state.compare_exchange_strong(prev, wait_slot::TIMED_OUT,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE)) {
          ret = -ETIMEDOUT;
          break;
        }
        static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
        ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
        ret = slot.handoff; slot.handoff = 0;
        break;
      }
    }

    if (ret != -ETIMEDOUT && ret != -EINTR) {
      // Waiter-side invalidation check: drain_waiters() or the pop-side
      // stale-slot handler clears wait_address to 0 before signaling us.
      // If we see 0, the mutex was destroyed/recycled while we slept —
      // bail out instead of returning to lock_slow where we'd operate
      // on recycled storage.
      bool invalidated = (slot.wait_address == 0);
      slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
      slot.wait_address = 0;
      if (invalidated)
        return finish(-EINVAL);
    }
    return finish(ret);
  }

  // ===== Pop-before-signal: lock-free Treiber stack pop =====
  //
  // Pop the top slot (CAS-32 on stack_), THEN signal it. Once popped,
  // the slot is detached from the stack — safe to signal and recycle.
  // No recycling race because no one reads slot.next after pop.
  //
  // Returns: true if a waiter was woken.

  LIBC_INLINE bool pop_and_signal_one() {
    for (;;) {
      uint32_t old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t top = stack_top(old_stk);
      if (top == STACK_NULL)
        return false;

      auto &slot = wait_slot::get_slot(top);

      // Read next BEFORE popping. The slot is still in our stack,
      // state is WAITING/IN_KERNEL/TIMED_OUT — safe to read.
      uint32_t next = slot.next;

      // Pop: advance stack top to next.
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1,
                                     static_cast<uint16_t>(next));
      if (!stack_.compare_exchange_weak(old_stk, new_stk,
                                         cpp::MemoryOrder::ACQ_REL,
                                         cpp::MemoryOrder::RELAXED))
        continue; // Concurrent push/pop — retry.

      // Popped! Slot is detached. Validate that this slot actually belongs
      // to this Futex instance. A stale slot from a destroyed/recycled
      // mutex will have wait_address == 0 (cleared by drain_waiters) or
      // pointing to a different Futex. Signal the stale waiter so it
      // wakes and bails out via the waiter-side invalidation check in
      // Futex::wait(). Do NOT reclaim — the owning thread is still alive
      // and holds a reference; it will clean up its own slot on wake.
      if (slot.wait_address != reinterpret_cast<uintptr_t>(this)) {
        slot.wait_address = 0; // Ensure invalidation is visible to waiter.
        uint8_t st = slot.state.exchange(wait_slot::SIGNALED,
                                          cpp::MemoryOrder::ACQ_REL);
        if (st == wait_slot::IN_KERNEL) {
          ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
              static_cast<uintptr_t>(slot.thread_id)));
        } else if (st == wait_slot::TIMED_OUT) {
          // Dead slot — safe to reclaim since no thread holds a ref.
          slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
          slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
          wait_slot::reclaim_slot(top);
        }
        continue;
      }

      // Single exchange replaces two CAS ops:
      // one xchg vs two lock cmpxchg in the IN_KERNEL hot path.
      uint8_t st = slot.state.exchange(wait_slot::SIGNALED,
                                        cpp::MemoryOrder::ACQ_REL);
      if (st == wait_slot::WAITING)
        return true; // Woke a WAITING waiter (cache-line write wakes monitor).

      if (st == wait_slot::IN_KERNEL) {
        ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(slot.thread_id)));
        return true; // Woke an IN_KERNEL waiter.
      }

      // Dead slot (TIMED_OUT, EINTR, or unexpected). Reclaim if possible.
      if (st == wait_slot::TIMED_OUT) {
        slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
        slot.wait_address = 0;
        slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
        wait_slot::reclaim_slot(top);
      } else {
        // Unexpected state — restore it.
        slot.state.store(st, cpp::MemoryOrder::RELAXED);
      }
      // Continue — pop the next slot.
    }
  }

  // ===== Wake one =====

  LIBC_INLINE long notify_one(bool is_shared = false) {
    (void)is_shared;
    // No has_waiters() pre-check — pop_and_signal_one already checks
    // stack_top == STACK_NULL on its first load. Avoids redundant ACQUIRE.
    pop_and_signal_one();
    return 0;
  }

  // ===== Wake all =====
  //
  // Steal the entire stack, then collect all TIDs into a flat array
  // and batch-alert via NtAlertMultipleThreadByThreadId. Signal all
  // slot states AFTER collecting TIDs so WAITING threads don't wake
  // and recycle their slots before we read their thread_ids.

  LIBC_INLINE long notify_all(bool is_shared = false) {
    (void)is_shared;

    // Steal the entire stack atomically (CAS-32 on stack_).
    uint32_t old_stk;
    for (;;) {
      old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      if (stack_top(old_stk) == STACK_NULL)
        return 0;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, STACK_NULL);
      if (stack_.compare_exchange_weak(old_stk, new_stk,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::RELAXED))
        break;
    }

    // Single-pass walk: exchange SIGNALED into every slot and collect
    // IN_KERNEL TIDs, then one batch alert. No chunking or yielding —
    // each yield/syscall risks a 15.6ms quantum loss under oversubscription,
    // and the kernel scheduler already paces dispatch to available LPs.
    // The walk itself (~3-5μs for 64 slots) is negligible.
    uint32_t curr = stack_top(old_stk);
    uint32_t num_alert = 0;
    uint32_t total_woken = 0;
    HANDLE alert_tids[128];

    while (curr != wait_slot::NULL_INDEX) {
      auto &slot = wait_slot::get_slot(curr);
      uint32_t next = slot.next;
      uint8_t old = slot.state.exchange(wait_slot::SIGNALED,
                                         cpp::MemoryOrder::ACQ_REL);
      if (old == wait_slot::IN_KERNEL) {
        alert_tids[num_alert++] = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(slot.thread_id));
        ++total_woken;
      } else if (old == wait_slot::WAITING) {
        ++total_woken;
      } else {
        // TIMED_OUT or unexpected — undo exchange + reclaim dead slots.
        slot.state.store(old == wait_slot::TIMED_OUT ? wait_slot::IDLE : old,
                         cpp::MemoryOrder::RELAXED);
        if (old == wait_slot::TIMED_OUT) {
          slot.wait_address = 0;
          slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
          wait_slot::reclaim_slot(curr);
        }
      }
      curr = next;
    }

    if (num_alert > 0)
      nt_optional().alert_multiple(alert_tids, num_alert, nullptr, 0);

    return static_cast<long>(total_woken);
  }

  // Test-only: notify_all with explicit chunk size and yield control.
  // chunk_size=0 means use LP count (same as notify_all).
  LIBC_INLINE long notify_all_chunked(uint32_t chunk_size,
                                       bool do_yield = true) {
    uint32_t old_stk;
    for (;;) {
      old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      if (stack_top(old_stk) == STACK_NULL)
        return 0;
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1, STACK_NULL);
      if (stack_.compare_exchange_weak(old_stk, new_stk,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::RELAXED))
        break;
    }

    uint32_t lp = chunk_size ? chunk_size
        : *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
    uint32_t curr = stack_top(old_stk);
    uint32_t total_woken = 0;
    HANDLE alert_tids[128];

    while (curr != wait_slot::NULL_INDEX) {
      uint32_t num_alert = 0;
      uint32_t chunk_live = 0;

      while (curr != wait_slot::NULL_INDEX && chunk_live < lp) {
        auto &slot = wait_slot::get_slot(curr);
        uint32_t next = slot.next;
        uint8_t old = slot.state.exchange(wait_slot::SIGNALED,
                                           cpp::MemoryOrder::ACQ_REL);
        if (old == wait_slot::IN_KERNEL) {
          alert_tids[num_alert++] = reinterpret_cast<HANDLE>(
              static_cast<uintptr_t>(slot.thread_id));
          ++chunk_live;
        } else if (old == wait_slot::WAITING) {
          ++chunk_live;
        } else {
          slot.state.store(old == wait_slot::TIMED_OUT ? wait_slot::IDLE : old,
                           cpp::MemoryOrder::RELAXED);
          if (old == wait_slot::TIMED_OUT) {
            slot.wait_address = 0;
            slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
            wait_slot::reclaim_slot(curr);
          }
        }
        curr = next;
      }

      total_woken += chunk_live;
      if (num_alert > 0) {
        nt_optional().alert_multiple(alert_tids, num_alert, nullptr, 0);
        if (do_yield && curr != wait_slot::NULL_INDEX && total_woken > lp)
          ::NtYieldExecution();
      }
    }

    return static_cast<long>(total_woken);
  }

  LIBC_INLINE long store_and_notify_all_chunked(FutexValueType new_val,
                                                 uint32_t chunk_size,
                                                 bool do_yield = true) {
    value_.store(new_val, cpp::MemoryOrder::SEQ_CST);
    return notify_all_chunked(chunk_size, do_yield);
  }

  // ===== Store-and-wake =====
  //
  // Store value (SEQ_CST = XCHG on x86 = full barrier), then pop+signal.
  // Any waiter mid-push sees the new value in their CAS-64 and fails
  // (value portion doesn't match). No lost wakeups.

  LIBC_INLINE long store_and_notify(FutexValueType new_val,
                                     bool is_shared = false) {
    (void)is_shared;
    value_.store(new_val, cpp::MemoryOrder::SEQ_CST);
    pop_and_signal_one();
    return 0;
  }

  // ===== Handoff wake (mutex-specific) =====
  //
  // Transfer ownership to the top waiter WITHOUT changing the value.
  // Pop-before-signal: pop the slot, set handoff=1, signal.
  // Returns true if handoff succeeded. If false (empty or dead),
  // caller must fall back to store_and_notify.

  LIBC_INLINE bool handoff_one(bool is_shared = false) {
    (void)is_shared;
    if (!has_waiters())
      return false;

    for (;;) {
      uint32_t old_stk = stack_.load(cpp::MemoryOrder::ACQUIRE);
      uint16_t top = stack_top(old_stk);
      if (top == STACK_NULL)
        return false;

      auto &slot = wait_slot::get_slot(top);
      uint32_t next = slot.next;

      // Pop first.
      uint32_t new_stk = stack_pack(stack_gen(old_stk) + 1,
                                     static_cast<uint16_t>(next));
      if (!stack_.compare_exchange_weak(old_stk, new_stk,
                                         cpp::MemoryOrder::ACQ_REL,
                                         cpp::MemoryOrder::RELAXED))
        continue;

      // Validate that this slot belongs to this Futex instance.
      // Stale slot from a destroyed/recycled mutex: signal it so the
      // owning thread wakes and bails out via waiter-side invalidation.
      // Do NOT reclaim — the owning thread cleans up its own slot.
      if (slot.wait_address != reinterpret_cast<uintptr_t>(this)) {
        slot.wait_address = 0;
        uint8_t st = slot.state.exchange(wait_slot::SIGNALED,
                                          cpp::MemoryOrder::ACQ_REL);
        if (st == wait_slot::IN_KERNEL) {
          ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
              static_cast<uintptr_t>(slot.thread_id)));
        } else if (st == wait_slot::TIMED_OUT) {
          slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
          slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
          wait_slot::reclaim_slot(top);
        }
        continue;
      }

      // Try handoff to WAITING target only (catches Phase 2.5 spin).
      uint8_t st = wait_slot::WAITING;
      slot.handoff = 1;
      if (slot.state.compare_exchange_strong(st, wait_slot::SIGNALED,
                                              cpp::MemoryOrder::RELEASE,
                                              cpp::MemoryOrder::ACQUIRE))
        return true;

      slot.handoff = 0;

      // Not WAITING — could be IN_KERNEL or TIMED_OUT.
      // For IN_KERNEL: handoff is unsafe (alert could be lost).
      // For TIMED_OUT: dead node.
      // Either way, don't handoff. Reclaim dead nodes, return false
      // so caller falls back to Dekker-safe store_and_notify.
      if (st == wait_slot::TIMED_OUT) {
        slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
        slot.wait_address = 0;
        slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
        wait_slot::reclaim_slot(top);
        continue; // Try next slot.
      }

      // IN_KERNEL — can't handoff safely. But we already popped it!
      // We need to either: (a) signal it via Dekker, or (b) re-push it.
      // Re-pushing is complex. Instead, signal it normally (the caller
      // will store the unlock value separately).
      if (st == wait_slot::IN_KERNEL &&
          slot.state.compare_exchange_strong(st, wait_slot::SIGNALED,
                                              cpp::MemoryOrder::RELEASE,
                                              cpp::MemoryOrder::ACQUIRE)) {
        ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(slot.thread_id)));
      }
      return false;
    }
  }

  // ===== Mutex unlock+wake (combined) =====
  //
  // Adaptive: tries handoff first (direct ownership transfer without
  // publishing UNLOCKED). Falls back to Dekker-safe value store + pop.
  //
  // For handoff: pops the slot, sets handoff=1, signals. Value unchanged.
  // For Dekker: stores unlock_val (SEQ_CST), then pops and signals.
  //
  // Returns true if a waiter was woken.

  LIBC_INLINE bool unlock_notify(FutexValueType unlock_val,
                                  bool is_shared = false) {
    (void)is_shared;

    // Try handoff first. If successful, value stays unchanged
    // (no thundering herd from spinners).
    if (handoff_one())
      return true;

    // Handoff failed (empty, dead, or IN_KERNEL). Store the unlock
    // value and pop+signal via Dekker path.
    value_.store(unlock_val, cpp::MemoryOrder::SEQ_CST);
    return pop_and_signal_one();
  }

  LIBC_INLINE long store_and_notify_all(FutexValueType new_val,
                                         bool is_shared = false) {
    (void)is_shared;
    value_.store(new_val, cpp::MemoryOrder::SEQ_CST);
    return notify_all();
  }
};

static_assert(__is_standard_layout(Futex),
              "Futex must be a standard layout type.");
static_assert(sizeof(Futex) == 8, "Futex must be 8 bytes.");

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_UTILS_H
