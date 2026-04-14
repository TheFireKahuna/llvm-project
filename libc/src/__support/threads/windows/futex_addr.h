//===--- Parking lot for address-keyed waits + shared utilities --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two wait/wake backends sharing the same slot pool and sleep/wake primitives:
//
// 1. Embedded per-Futex Treiber stack — CAS-64 push, CAS-32 pop.
//    Lock-free. Lives entirely in futex_utils.h / Futex class methods.
//
// 2. Address-keyed parking lot — 256 cache-line-aligned buckets for
//    futex_addr::wait/wake on arbitrary memory addresses. Uses per-bucket
//    spinlocks with locked insert + Dekker re-check.
//
// This file provides:
//   - cpu_relax(), spin_on_value(), timeout conversion (shared utilities)
//   - Parking lot (Bucket, hash, wait/wake for address-keyed waits)
//   - Batch alert helpers
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_ADDR_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_ADDR_H

#include "hdr/types/struct_timespec.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/threads/windows/wait_slot.h"

#include <errno.h>

namespace LIBC_NAMESPACE_DECL {
namespace futex_addr {

//===----------------------------------------------------------------------===//
// Architecture-portable spin hint
//===----------------------------------------------------------------------===//

LIBC_INLINE void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ volatile("yield" ::: "memory");
#else
  __asm__ volatile("" ::: "memory");
#endif
}

//===----------------------------------------------------------------------===//
// Parking lot (address-keyed waits)
//===----------------------------------------------------------------------===//

static constexpr unsigned BUCKET_COUNT = 256;

// Adaptive TTAS spinlock with exponential backoff.
// Phase 1: single exchange (fast uncontended path).
// Phase 2: read-spin keeps line in Shared state, budget doubles 4→128.
// Phase 3: NtYieldExecution after each budget exhaustion — prevents CPU
//   starvation on SMT cores under sustained contention.
struct BucketLock {
  cpp::Atomic<uint8_t> flag{0};

  LIBC_INLINE void acquire() {
    if (flag.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0)
      return;
    // Quick retry before full TTAS loop (see wl_lock comment).
    for (int retry = 0; retry < 2; ++retry) {
      cpu_relax();
      if (flag.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0)
        return;
    }
    for (unsigned outer = 0;; ++outer) {
      unsigned spin_limit = 4u << (outer < 5 ? outer : 5);
      bool saw_unlocked = false;
      for (unsigned i = 0; i < spin_limit; ++i) {
        if (flag.load(cpp::MemoryOrder::RELAXED) == 0) {
          saw_unlocked = true;
          break;
        }
        cpu_relax();
      }
      if (!saw_unlocked)
        ::NtYieldExecution();
      if (flag.exchange(1, cpp::MemoryOrder::ACQUIRE) == 0)
        return;
    }
  }

  LIBC_INLINE void release() {
    flag.store(0, cpp::MemoryOrder::RELEASE);
  }
};

// Cache-line-aligned to prevent false sharing between adjacent buckets.
// live_count enables the lock-free empty-bucket fast path: wakers skip
// the lock entirely when zero. Exact — incremented on insert (under lock),
// decremented on wake claim (under lock) or timeout CAS (lock-free).
struct alignas(64) Bucket {
  BucketLock lock;
  uint32_t head{0}; // SLL head, protected by lock (not atomic — lock held)
  cpp::Atomic<uint32_t> live_count{0};
};

inline Bucket &get_bucket(unsigned index) {
  static Bucket buckets[BUCKET_COUNT];
  return buckets[index];
}

LIBC_INLINE unsigned hash_address(uintptr_t addr) {
  addr >>= 4;
  addr ^= addr >> 16;
  addr ^= addr >> 8;
  return static_cast<unsigned>(addr) & (BUCKET_COUNT - 1);
}

//===----------------------------------------------------------------------===//
// DLL operations (all called under bucket lock)
//===----------------------------------------------------------------------===//

LIBC_INLINE void sll_insert(Bucket &bucket, uint32_t idx) {
  auto &slot = wait_slot::get_slot(idx);
  slot.next = bucket.head;
  bucket.head = idx;
}

LIBC_INLINE void sll_remove(Bucket &bucket, uint32_t idx) {
  if (bucket.head == idx) {
    bucket.head = wait_slot::get_slot(idx).next;
    return;
  }
  uint32_t prev = bucket.head;
  while (prev != wait_slot::NULL_INDEX) {
    uint32_t next = wait_slot::get_slot(prev).next;
    if (next == idx) {
      wait_slot::get_slot(prev).next = wait_slot::get_slot(idx).next;
      return;
    }
    prev = next;
  }
}

//===----------------------------------------------------------------------===//
// Spin
//===----------------------------------------------------------------------===//

template <typename T>
LIBC_INLINE bool spin_on_value(const volatile T *addr, T expected) {
  for (unsigned i = 0; i < 32; ++i) {
    if (__atomic_load_n(addr, __ATOMIC_RELAXED) != expected)
      return true;
    cpu_relax();
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Timeout conversion
//===----------------------------------------------------------------------===//

LIBC_INLINE LARGE_INTEGER *
timespec_to_nt(const struct timespec *timeout, LARGE_INTEGER &storage) {
  if (!timeout)
    return nullptr;
  long long hns = static_cast<long long>(timeout->tv_sec) * 10'000'000LL +
                  timeout->tv_nsec / 100;
  // Clamp negative (bogus) to 1 tick — prevents infinite waits.
  if (hns < 0)
    hns = 1;
  storage.QuadPart = -hns; // NT relative timeout is negative
  return &storage;
}

//===----------------------------------------------------------------------===//
// Deferred stale slot reclamation
//===----------------------------------------------------------------------===//
//
// Timeout cancellation is lock-free (CAS IN_KERNEL → TIMED_OUT). The slot
// stays linked until the next wake scan lazily cleans it, or the owning
// thread calls this before its next wait. Deferred from the timeout path
// (latency-sensitive) to the wait-entry path (not latency-critical).

LIBC_INLINE void reclaim_stale_slot() {
  uintptr_t stale_addr;
  uint32_t stale_idx = wait_slot::get_stale_slot(stale_addr);
  if (stale_idx == wait_slot::NULL_INDEX)
    return;

  unsigned bi = hash_address(stale_addr);
  Bucket &b = get_bucket(bi);
  b.lock.acquire();
  auto &slot = wait_slot::get_slot(stale_idx);
  // Re-check under lock: a wake scan may have already reclaimed and recycled
  // the slot. Only touch it if still TIMED_OUT — otherwise the slot may
  // belong to a different live waiter (TOCTOU between get_stale_slot and here).
  if (slot.state.load(cpp::MemoryOrder::RELAXED) == wait_slot::TIMED_OUT) {
    sll_remove(b, stale_idx);
    // live_count already decremented by the timeout path (lock-free CAS).
    slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
    slot.wait_address = 0;
  }
  b.lock.release();
}

//===----------------------------------------------------------------------===//
// Wait (address-keyed, parking lot)
//===----------------------------------------------------------------------===//

template <typename T = uint32_t, bool Interruptible = false>
inline long wait_nt(const volatile T *addr, T expected,
                    LARGE_INTEGER *nt_timeout) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 ||
                sizeof(T) == 4 || sizeof(T) == 8,
                "futex_addr::wait requires 1, 2, 4, or 8 byte type");

  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
    return -EAGAIN;

  if (spin_on_value(addr, expected))
    return -EAGAIN;

  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
    return -EAGAIN;

  reclaim_stale_slot();

  uint32_t my_idx = wait_slot::get_slot_index();
  if (my_idx == wait_slot::NULL_INDEX)
    return -EAGAIN;

  auto &slot = wait_slot::get_slot(my_idx);
  slot.wait_address = reinterpret_cast<uintptr_t>(addr);
  slot.state.store(wait_slot::WAITING, cpp::MemoryOrder::RELAXED);

  unsigned bucket_idx = hash_address(reinterpret_cast<uintptr_t>(addr));
  Bucket &bucket = get_bucket(bucket_idx);

  // Insert under lock + Dekker re-check.
  //
  // The lock makes insert + live_count++ atomic from the waker's view.
  // Dekker ordering (waiter side):
  //   WRITE live_count (under lock) → unlock (RELEASE) → fence → READ value
  // Combined with the waker's:
  //   WRITE value → fence → READ live_count
  // The SEQ_CST fences create a total order ensuring at least one side
  // sees the other's write.
  bucket.lock.acquire();
  bucket.live_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  sll_insert(bucket, my_idx);
  // Re-check value under lock. If changed, undo immediately — no dead node.
  if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected) {
    sll_remove(bucket, my_idx);
    bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
    bucket.lock.release();
    slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
    slot.wait_address = 0;
    return -EAGAIN;
  }
  bucket.lock.release();

  // Pre-kernel spin: monitor slot state for a wake claim while still
  // in WAITING (see Futex::wait Phase 2.5 for rationale).
  if (spin_wait::spin_on_slot_state(&slot.state, wait_slot::WAITING)) {
    slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
    slot.wait_address = 0;
    return 0;
  }

  // Transition WAITING → IN_KERNEL. If a waker claimed us between the
  // spin exit and here (WAITING → SIGNALED), skip the kernel sleep.
  {
    uint8_t prev = wait_slot::WAITING;
    if (!slot.state.compare_exchange_strong(prev, wait_slot::IN_KERNEL,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::ACQUIRE)) {
      slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
      slot.wait_address = 0;
      return 0;
    }
  }

  // Kernel sleep.
  PVOID tid_ptr =
      reinterpret_cast<PVOID>(static_cast<uintptr_t>(slot.thread_id));

  long ret = 0;
  for (;;) {
    uint8_t st = slot.state.load(cpp::MemoryOrder::ACQUIRE);
    if (st != wait_slot::IN_KERNEL) {
      ret = 0;
      break;
    }

    NTSTATUS status = ::NtWaitForAlertByThreadId(tid_ptr, nt_timeout);

    st = slot.state.load(cpp::MemoryOrder::ACQUIRE);
    if (st == wait_slot::SIGNALED) {
      // If NtWaitForAlertByThreadId didn't consume the alert token
      // (returned APC/spurious, not STATUS_ALERTED), drain it to prevent
      // a stale alert on the next wait.
      if (status != STATUS_ALERTED) {
        static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
        ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
      }
      ret = 0;
      break;
    }

    if (status == STATUS_TIMEOUT) {
      // Lock-free timeout: CAS IN_KERNEL → TIMED_OUT. Only one of
      // {timeout, wake claim} can succeed per slot — no double-decrement.
      uint8_t prev = wait_slot::IN_KERNEL;
      if (slot.state.compare_exchange_strong(prev, wait_slot::TIMED_OUT,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE)) {
        bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        ret = -ETIMEDOUT;
        break;
      }
      // Waker raced — claimed us. Drain pending alert.
      static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
      ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
      ret = 0;
      break;
    }
    // Spurious wake (APC / stale alert / kernel noise).
    if constexpr (Interruptible) {
      // Signal APC — self-cancel (CAS IN_KERNEL → TIMED_OUT) and
      // return -EINTR. The waker handles TIMED_OUT slots identically
      // to timeout: reclaims slot during lazy wake scan.
      uint8_t prev = wait_slot::IN_KERNEL;
      if (slot.state.compare_exchange_strong(prev, wait_slot::TIMED_OUT,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE)) {
        bucket.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);
        ret = -EINTR;
        break;
      }
      // Waker raced — already set SIGNALED. Drain the pending alert
      // and return as a normal wake.
      static constexpr LARGE_INTEGER DRAIN_TIMEOUT = {.QuadPart = -1};
      ::NtWaitForAlertByThreadId(tid_ptr, &DRAIN_TIMEOUT);
      ret = 0;
      break;
    }
    // Non-interruptible: loop re-sleeps.
  }

  // SIGNALED: waker already unlinked us. TIMED_OUT / EINTR: slot stays
  // linked, cleaned up by reclaim_stale_slot on next wait or lazy wake scan.
  if (ret != -ETIMEDOUT && ret != -EINTR) {
    slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
    slot.wait_address = 0;
  }
  return ret;
}

template <typename T = uint32_t, bool Interruptible = false>
inline long wait_nt(const volatile cpp::Atomic<T> *addr, T expected,
                    LARGE_INTEGER *nt_timeout) {
  return wait_nt<T, Interruptible>(&addr->val, expected, nt_timeout);
}

template <typename T = uint32_t, bool Interruptible = false>
inline long wait(const volatile T *addr, T expected,
                 const struct timespec *timeout) {
  LARGE_INTEGER nt_storage;
  LARGE_INTEGER *nt_timeout = timespec_to_nt(timeout, nt_storage);
  return wait_nt<T, Interruptible>(addr, expected, nt_timeout);
}

template <typename T = uint32_t, bool Interruptible = false>
inline long wait(const volatile cpp::Atomic<T> *addr, T expected,
                 const struct timespec *timeout) {
  LARGE_INTEGER nt_storage;
  LARGE_INTEGER *nt_timeout = timespec_to_nt(timeout, nt_storage);
  return wait_nt<T, Interruptible>(addr, expected, nt_timeout);
}

//===----------------------------------------------------------------------===//
// Batch alert
//===----------------------------------------------------------------------===//

// 64 HANDLEs × 8 bytes = 512 bytes — avoids page_alloc syscall for
// common fan-out sizes. The batch API is 2.5x faster than individual
// alerts at 64 threads, so we want to stay on the stack path.
static constexpr uint32_t BATCH_STACK_LIMIT = 64;

LIBC_INLINE void batch_alert(uint32_t alert_head, uint32_t count) {
  if (alert_head == wait_slot::NULL_INDEX)
    return;

  if (count == 1) {
    ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(
            wait_slot::get_slot(alert_head).thread_id)));
    return;
  }

  // Small fanout: collect on stack to avoid page_alloc overhead.
  if (count <= BATCH_STACK_LIMIT) {
    HANDLE stack_buf[BATCH_STACK_LIMIT];
    uint32_t n = 0;
    uint32_t curr = alert_head;
    while (curr != wait_slot::NULL_INDEX && n < count) {
      stack_buf[n++] = reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(wait_slot::get_slot(curr).thread_id));
      curr = wait_slot::get_slot(curr).next;
    }
    if (n > 0)
      nt_optional().alert_multiple(stack_buf, n, nullptr, 0);
    return;
  }

  // Large fanout: page-allocate TID buffer.
  size_t buf_bytes = static_cast<size_t>(count) * sizeof(HANDLE);
  auto *tid_buf = static_cast<HANDLE *>(internal::page_alloc(buf_bytes));

  if (tid_buf) {
    uint32_t n = 0;
    uint32_t curr = alert_head;
    while (curr != wait_slot::NULL_INDEX && n < count) {
      tid_buf[n++] = reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(wait_slot::get_slot(curr).thread_id));
      curr = wait_slot::get_slot(curr).next;
    }
    if (n > 0)
      nt_optional().alert_multiple(tid_buf, n, nullptr, 0);
    internal::page_free(tid_buf);
    return;
  }

  // Fallback on alloc failure: alert one at a time.
  uint32_t curr = alert_head;
  while (curr != wait_slot::NULL_INDEX) {
    auto &slot = wait_slot::get_slot(curr);
    uint32_t next = slot.next;
    ::NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(slot.thread_id)));
    curr = next;
  }
}

//===----------------------------------------------------------------------===//
// Wake (address-keyed, parking lot)
//===----------------------------------------------------------------------===//
//
// Single-pass walk under lock: claim matching live slots, lazily unlink
// dead (TIMED_OUT) slots encountered during traversal. Claimed slots are
// unlinked from the SLL. Alerts happen outside the lock.

// FIFO wake for parking lot: when count==1, wake the oldest matching
// waiter (tail of SLL) to prevent starvation. When count>1, wake the
// oldest `count` waiters. Full walk needed anyway for lazy cleanup.
inline long wake(const volatile void *addr, uint32_t count) {
  if (count == 0)
    return 0;

  uintptr_t target = reinterpret_cast<uintptr_t>(addr);
  unsigned bucket_idx = hash_address(target);
  Bucket &bucket = get_bucket(bucket_idx);

  // Dekker fence: caller wrote the value — ensure globally visible before
  // we read live_count. Pairs with waiter's live_count write under lock.
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (bucket.live_count.load(cpp::MemoryOrder::SEQ_CST) == 0)
    return 0;

  uint32_t dead_head = wait_slot::NULL_INDEX;

  bucket.lock.acquire();

  // Phase 1: full walk — clean dead slots, collect matching candidates.
  // We record up to `count` candidates in FIFO order (oldest first).
  // Candidates are indices of live matching slots, not yet claimed.
  // 64 is generous — matches BATCH_STACK_LIMIT.
  struct Candidate {
    uint32_t idx;
    uint32_t prev; // for O(1) unlink
  };
  Candidate candidates[64];
  uint32_t num_candidates = 0;

  uint32_t prev_idx = wait_slot::NULL_INDEX;
  uint32_t curr = bucket.head;
  while (curr != wait_slot::NULL_INDEX) {
    auto &slot = wait_slot::get_slot(curr);
    uint32_t next = slot.next;

    uint8_t st = slot.state.load(cpp::MemoryOrder::RELAXED);

    // Lazy cleanup of dead slots from lock-free timeouts.
    if (st == wait_slot::TIMED_OUT) {
      // Unlink from SLL.
      if (prev_idx != wait_slot::NULL_INDEX)
        wait_slot::get_slot(prev_idx).next = next;
      else
        bucket.head = next;
      slot.state.store(wait_slot::IDLE, cpp::MemoryOrder::RELAXED);
      slot.wait_address = 0;
      slot.generation.fetch_add(1, cpp::MemoryOrder::RELEASE);
      slot.next = dead_head;
      dead_head = curr;
      // Don't advance prev — curr was removed.
      curr = next;
      continue;
    }

    if (slot.wait_address == target &&
        (st == wait_slot::WAITING || st == wait_slot::IN_KERNEL)) {
      // Record candidate. Newer candidates shift older ones out when
      // we exceed count — we want the oldest `count` (tail of SLL).
      if (num_candidates < count && num_candidates < 64) {
        candidates[num_candidates++] = {curr, prev_idx};
      }
      // If we already have `count` candidates, the newest ones at
      // the head aren't needed. But SLL is head=newest, so we're
      // walking newest→oldest. We want oldest. Keep collecting —
      // the last `count` candidates are the oldest.
      // Actually, since SLL head is newest: as we walk, each match
      // is older than the last. So the first match is newest, last
      // match is oldest. For FIFO we want the oldest `count`.
      // Strategy: collect ALL matches, then take the last `count`.
      else if (num_candidates < 64) {
        candidates[num_candidates++] = {curr, prev_idx};
      }
    }

    prev_idx = curr;
    curr = next;
  }

  // Take the oldest `count` candidates (last entries in the array).
  uint32_t start = (num_candidates > count) ? num_candidates - count : 0;
  uint32_t alert_head = wait_slot::NULL_INDEX;
  uint32_t alert_count = 0;
  long woken = 0;

  // Claim oldest-first. Unlink in reverse order to keep prev pointers
  // valid (unlinking tail-ward doesn't invalidate earlier prevs).
  for (uint32_t i = num_candidates; i > start;) {
    --i;
    auto &slot = wait_slot::get_slot(candidates[i].idx);
    uint8_t prev_state = wait_slot::WAITING;
    if (slot.state.compare_exchange_strong(prev_state, wait_slot::SIGNALED,
                                           cpp::MemoryOrder::RELEASE,
                                           cpp::MemoryOrder::ACQUIRE)) {
      uint32_t next = slot.next;
      if (candidates[i].prev != wait_slot::NULL_INDEX)
        wait_slot::get_slot(candidates[i].prev).next = next;
      else
        bucket.head = next;
      ++woken;
      continue;
    }
    if (prev_state == wait_slot::IN_KERNEL &&
        slot.state.compare_exchange_strong(
            prev_state, wait_slot::SIGNALED,
            cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::ACQUIRE)) {
      uint32_t next = slot.next;
      if (candidates[i].prev != wait_slot::NULL_INDEX)
        wait_slot::get_slot(candidates[i].prev).next = next;
      else
        bucket.head = next;
      slot.next = alert_head;
      alert_head = candidates[i].idx;
      ++alert_count;
      ++woken;
    }
  }

  if (woken > 0)
    bucket.live_count.fetch_sub(static_cast<uint32_t>(woken),
                                cpp::MemoryOrder::RELAXED);
  bucket.lock.release();

  // Reclaim dead slots outside lock — return to freelist.
  uint32_t dead = dead_head;
  while (dead != wait_slot::NULL_INDEX) {
    uint32_t next = wait_slot::get_slot(dead).next;
    wait_slot::reclaim_slot(dead);
    dead = next;
  }

  // Batch-alert IN_KERNEL waiters — syscall outside lock.
  batch_alert(alert_head, alert_count);
  return woken;
}

template <typename T>
inline long wake(const volatile cpp::Atomic<T> *addr, uint32_t count) {
  return wake(&addr->val, count);
}

} // namespace futex_addr
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_ADDR_H
