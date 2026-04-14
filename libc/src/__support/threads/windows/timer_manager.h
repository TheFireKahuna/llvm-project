//===-- POSIX timer infrastructure for Windows -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements POSIX per-process timers (timer_create et al.) on Windows using
// NtCreateTimer2 for the kernel timer objects and the process-wide reactor
// (reactor::watch) for notification delivery. Each timer handle is watched
// via WaitCompletionPacket on the reactor's IOCP; the drain thread dispatches
// callbacks when the timer fires.
//
// Timer slots are backed by IndexedPool — guard-page-hardened, chunk-level
// decommit, occupancy bitmap. timer_t is an opaque integer index (not a raw
// pointer), eliminating address leaks and enabling bounds/liveness validation.
//
// Design:
//   SIGEV_NONE   — pure kernel timer, no waiter, query-only
//   SIGEV_SIGNAL — reactor callback delivers via signal subsystem
//   SIGEV_THREAD — reactor callback spawns user function thread
//
// Overrun tracking uses time-based computation: when the callback fires,
// overrun = floor((now - expected_fire) / period) for periodic timers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_TIMER_MANAGER_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_TIMER_MANAGER_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/indexed_pool.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/OSUtil/windows/signal/signal.h"

#include <signal.h>
#include <time.h>

namespace LIBC_NAMESPACE_DECL {
namespace timer_manager {

enum SlotState : int {
  SLOT_FREE = 0,
  SLOT_ALLOCATED = 1,
  SLOT_ARMED = 2,
};

struct TimerSlot {
  HANDLE nt_timer;
  internal::reactor::ReactorToken reactor_token{internal::reactor::INVALID_TOKEN};

  // POSIX configuration — immutable after timer_create, safe to read from
  // concurrent callback threads without synchronization.
  clockid_t clock_id;
  int sigev_notify;
  int sigev_signo;
  union sigval sigev_value;
  void (*sigev_notify_function)(union sigval);
  SIZE_T thread_stack_size; // From sigev_notify_attributes, 0 = default

  // Timing state for overrun computation.
  long long interval_hns;  // Period in 100ns, 0 = one-shot
  long long next_fire_hns; // Expected next firing (clock domain 100ns)

  cpp::Atomic<int> overrun;
  cpp::Atomic<int> state;

  // Pool index for this slot. Set during allocation, used by callbacks
  // (e.g., si_tid in signal delivery) and for index-based free_slot.
  unsigned pool_index;
};

// IndexedPool-backed timer allocation. ChunkShift=8 → 256 timers per chunk.
// Grows dynamically — no fixed ceiling. Physical memory returned to the OS
// when all timers in a chunk are deleted (chunk-level decommit).
using TimerPool = internal::IndexedPool<TimerSlot, 8>;
inline TimerPool timer_pool;
inline cpp::Atomic<uint8_t> pool_init_state{0};

inline constexpr uint8_t POOL_INIT_UNINITIALIZED = 0;
inline constexpr uint8_t POOL_INIT_IN_PROGRESS = 1;
inline constexpr uint8_t POOL_INIT_READY = 2;

inline void ensure_pool_init() {
  for (;;) {
    uint8_t state = pool_init_state.load(cpp::MemoryOrder::ACQUIRE);
    if (LIBC_LIKELY(state == POOL_INIT_READY))
      return;

    if (state == POOL_INIT_UNINITIALIZED) {
      uint8_t expected = POOL_INIT_UNINITIALIZED;
      if (!pool_init_state.compare_exchange_strong(expected,
                                                   POOL_INIT_IN_PROGRESS,
                                                   cpp::MemoryOrder::ACQ_REL,
                                                   cpp::MemoryOrder::ACQUIRE))
        continue;

      timer_pool.init();
      pool_init_state.store(POOL_INIT_READY, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(&pool_init_state, static_cast<uint32_t>(-1));
      return;
    }

    futex_addr::wait(&pool_init_state, POOL_INIT_IN_PROGRESS, nullptr);
  }
}

// timer_t encoding: index + 1, so timer index 0 maps to non-null timer_t.
// timer_t(0) / NULL is reserved as "invalid timer".
inline timer_t index_to_timer(unsigned idx) {
  return reinterpret_cast<timer_t>(static_cast<uintptr_t>(idx + 1));
}

inline int timer_to_index(timer_t t) {
  auto v = reinterpret_cast<uintptr_t>(t);
  return (v == 0) ? -1 : static_cast<int>(v - 1);
}

// Allocate a free timer slot. Returns the pool index, or -1 on failure.
// Scans existing chunks via the occupancy bitmap, extends with a new
// chunk if all are full. The slot's state is set to SLOT_ALLOCATED and
// all other fields are zero-initialized (OS zero-fills on commit/recommit).
inline int alloc_slot() {
  ensure_pool_init();

  for (unsigned ci = 0;; ++ci) {
    TimerSlot *chunk = timer_pool.acquire_for_scan(ci);
    if (!chunk)
      return -1; // OOM

    auto *meta = TimerPool::meta_for(chunk);

    for (unsigned w = 0; w < TimerPool::BITMAP_WORDS; ++w) {
      uint64_t bits = meta->bitmap[w].load(cpp::MemoryOrder::ACQUIRE);
      uint64_t free_bits = ~bits;
      while (free_bits) {
        unsigned b = static_cast<unsigned>(__builtin_ctzll(free_bits));
        unsigned local = w * 64 + b;
        TimerSlot *slot = &chunk[local];

        int expected = SLOT_FREE;
        if (slot->state.compare_exchange_strong(expected, SLOT_ALLOCATED,
                                                cpp::MemoryOrder::ACQ_REL,
                                                cpp::MemoryOrder::RELAXED)) {
          unsigned idx = (ci << TimerPool::CHUNK_SHIFT) + local;
          slot->pool_index = idx;
          timer_pool.mark_live_bitmap_only(idx);
          return static_cast<int>(idx);
        }
        free_bits &= free_bits - 1; // blsr — clear lowest set bit
      }
    }

    // Chunk was full — release pin, try next.
    timer_pool.release_scan_ref(ci);
  }
}

// Free a timer slot by pool index. Clears state and releases the slot
// back to the pool, potentially triggering chunk decommit if the chunk
// becomes fully empty.
inline void free_slot(unsigned idx) {
  TimerSlot *slot = timer_pool.slot_for(idx);
  if (slot)
    slot->state.store(SLOT_FREE, cpp::MemoryOrder::RELEASE);
  timer_pool.mark_dead(idx);
}

inline bool is_alarm_clock(clockid_t clock_id) {
  return clock_id == CLOCK_REALTIME_ALARM || clock_id == CLOCK_BOOTTIME_ALARM;
}

inline long long now_hns(clockid_t clock_id) {
  constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;
  switch (clock_id) {
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_COARSE:
  case CLOCK_REALTIME_ALARM:
    return ::RtlGetSystemTimePrecise() - EPOCH_DIFF_HNS;
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE: {
    ULONGLONG t;
    ::RtlQueryUnbiasedInterruptTime(&t);
    return static_cast<long long>(t);
  }
  case CLOCK_BOOTTIME:
  case CLOCK_BOOTTIME_ALARM: {
    ULONGLONG t;
    ::QueryInterruptTime(&t);
    return static_cast<long long>(t);
  }
  default:
    return 0;
  }
}

inline long long timespec_to_hns(const struct timespec &ts) {
  return static_cast<long long>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100;
}

inline void hns_to_timespec(long long hns, struct timespec &ts) {
  if (hns <= 0) {
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    return;
  }
  ts.tv_sec = static_cast<decltype(ts.tv_sec)>(hns / 10000000LL);
  ts.tv_nsec = static_cast<decltype(ts.tv_nsec)>((hns % 10000000LL) * 100);
}

inline LARGE_INTEGER compute_due_time(clockid_t clock_id, long long target_hns,
                                      int flags) {
  LARGE_INTEGER due;
  constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;

  if (flags & TIMER_ABSTIME) {
    switch (clock_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_REALTIME_ALARM:
      due.QuadPart = target_hns + EPOCH_DIFF_HNS;
      return due;
    default: {
      long long delta = target_hns - now_hns(clock_id);
      due.QuadPart = (delta > 0) ? -delta : -1;
      return due;
    }
    }
  }
  due.QuadPart = (target_hns > 0) ? -target_hns : -1;
  return due;
}

// SIGEV_THREAD entry point. Receives the TimerSlot pointer, reads the
// immutable sigev fields, and calls the user function. The slot is
// guaranteed stable: timer_delete blocks on reactor::unwatch() until
// all in-flight callbacks (including this thread's parent callback) complete,
// and the sigevent fields are immutable after timer_create.
inline DWORD WINAPI sigev_thread_entry(PVOID arg) {
  auto *slot = static_cast<TimerSlot *>(arg);
  // Copy to locals before calling — defensive against slot reuse in
  // pathological timer_delete + timer_create sequences, though the
  // current reactor::unwatch() blocking semantics prevent this.
  void (*fn)(union sigval) = slot->sigev_notify_function;
  union sigval val = slot->sigev_value;
  fn(val);
  return 0;
}

// Reactor callback — fires on the drain thread when the NT timer handle
// becomes signaled. For SynchronizationTimer, the signal is consumed
// atomically by the WCP association, preventing double-delivery.
// Periodic timers re-arm via reactor::rearm() at the end.
inline void timer_reactor_cb(void *context, NTSTATUS /*status*/,
                             ULONG_PTR /*information*/) {
  auto *slot = static_cast<TimerSlot *>(context);

  if (slot->state.load(cpp::MemoryOrder::ACQUIRE) != SLOT_ARMED)
    return;

  // Compute overruns for periodic timers.
  int overruns = 0;
  if (slot->interval_hns > 0) {
    long long current = now_hns(slot->clock_id);
    long long expected = slot->next_fire_hns;
    if (current > expected) {
      overruns =
          static_cast<int>((current - expected) / slot->interval_hns);
      slot->next_fire_hns =
          expected + static_cast<long long>(overruns + 1) * slot->interval_hns;
    } else {
      slot->next_fire_hns = expected + slot->interval_hns;
    }
  } else {
    // One-shot: transition back to allocated.
    slot->state.store(SLOT_ALLOCATED, cpp::MemoryOrder::RELEASE);
  }

  slot->overrun.store(overruns, cpp::MemoryOrder::RELEASE);

  switch (slot->sigev_notify) {
  case SIGEV_SIGNAL: {
    siginfo_t info{};
    info.si_signo = slot->sigev_signo;
    info.si_code = SI_TIMER;
    // si_tid = timer index (matches the integer encoded in timer_t).
    info._sifields._timer.si_tid = static_cast<int>(slot->pool_index);
    info._sifields._timer._overrun = overruns;
    info._sifields._timer.si_sigval = slot->sigev_value;
    signal_state::deliver_signal(slot->sigev_signo, &info);
    break;
  }
  case SIGEV_THREAD: {
    // POSIX: "a new thread is created" to run the function. The slot
    // pointer is the thread parameter — the entry function reads the
    // immutable sigevent fields directly. No heap allocation needed.
    HANDLE th = nullptr;
    SIZE_T stack_reserve = slot->thread_stack_size;
    NTSTATUS nst = ::NtCreateThreadEx(
        &th, SYNCHRONIZE, nullptr, NtCurrentProcess(),
        reinterpret_cast<PVOID>(sigev_thread_entry), slot, 0, 0, 0,
        stack_reserve, nullptr);
    if (NT_SUCCESS(nst) && th)
      ::NtClose(th); // Detached — we don't join SIGEV_THREAD threads.
    break;
  }
  case SIGEV_NONE:
  default:
    break;
  }

  // Re-arm the reactor watch for periodic timers. For one-shot timers
  // the NT timer won't signal again, so rearm is harmless (WCP waits
  // on a handle that will never fire until the next NtSetTimer2 call).
  if (slot->interval_hns > 0)
    internal::reactor::rearm(slot->reactor_token);
}

// Fork reinit: invalidate all live timer slots in the child process.
// NT timer handles were created with internal_oa() (non-inheritable),
// so they don't exist in the child's handle table — just null them.
// The pool decommits empty chunks at the end.
inline void fork_reinit() {
  uint8_t init_state = pool_init_state.load(cpp::MemoryOrder::ACQUIRE);
  if (init_state == POOL_INIT_UNINITIALIZED)
    return; // Pool never initialized — nothing to clean up.

  if (init_state == POOL_INIT_IN_PROGRESS) {
    pool_init_state.store(POOL_INIT_UNINITIALIZED, cpp::MemoryOrder::RELAXED);
    return;
  }

  // Safe to call mark_dead inside for_each_live: for_each_live pins each
  // chunk (live_count + 1) before iterating, so mark_dead decrements
  // toward 1 (the pin) but never reaches 0. Decommit only triggers when
  // the pin is released after the chunk iteration completes.
  timer_pool.for_each_live(
      [](unsigned idx, TimerSlot *slot, void *) {
        // NT timer handle is non-inherited — just null the stale pointer.
        slot->nt_timer = nullptr;
        // Reactor tokens are already invalidated by reactor's fork_reinit.
        slot->reactor_token = internal::reactor::INVALID_TOKEN;
        slot->state.store(SLOT_FREE, cpp::MemoryOrder::RELAXED);
        timer_pool.mark_dead(idx);
      },
      nullptr);

  timer_pool.fork_reinit();
}

} // namespace timer_manager
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_TIMER_MANAGER_H
