//===-- Windows internal timer operations ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for POSIX timer operations on Windows. These
// implement Linux syscall semantics: 0 on success, -errno on failure
// (timer_getoverrun returns count >= 0 on success, -errno on failure).
// Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "timer_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/time_macros.h"
#include "hdr/types/struct_itimerspec.h"
#include "hdr/types/struct_sigevent.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/threads/windows/timer_manager.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t timer_create(clockid_t clockid, struct sigevent *__restrict sevp,
                  timer_t *__restrict timerid) {
  // Validate clock.
  switch (clockid) {
  case CLOCK_REALTIME:
  case CLOCK_MONOTONIC:
  case CLOCK_BOOTTIME:
  case CLOCK_REALTIME_COARSE:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE:
  case CLOCK_REALTIME_ALARM:
  case CLOCK_BOOTTIME_ALARM:
    break;
  default:
    return -EINVAL;
  }

  // Allocate a timer slot — returns the pool index.
  int idx = timer_manager::alloc_slot();
  if (idx < 0)
    return -EAGAIN;

  auto *slot = timer_manager::timer_pool.slot_for(static_cast<unsigned>(idx));

  // Determine notification method. POSIX: if sevp is NULL, default is
  // SIGEV_SIGNAL with sigev_signo=SIGALRM and sigev_value=timer_id.
  int notify = SIGEV_SIGNAL;
  int signo = SIGALRM;
  union sigval sival;
  sival.sival_ptr = timer_manager::index_to_timer(static_cast<unsigned>(idx));
  void (*notify_fn)(union sigval) = nullptr;

  if (sevp) {
    notify = sevp->sigev_notify;
    signo = sevp->sigev_signo;
    sival = sevp->sigev_value;
    notify_fn = sevp->sigev_notify_function;
  }

  // Validate notification type.
  switch (notify) {
  case SIGEV_NONE:
  case SIGEV_SIGNAL:
  case SIGEV_THREAD:
    break;
  default:
    timer_manager::free_slot(static_cast<unsigned>(idx));
    return -EINVAL;
  }

  if (notify == SIGEV_SIGNAL && (signo < 1 || signo >= NSIG)) {
    timer_manager::free_slot(static_cast<unsigned>(idx));
    return -EINVAL;
  }

  if (notify == SIGEV_THREAD && !notify_fn) {
    timer_manager::free_slot(static_cast<unsigned>(idx));
    return -EINVAL;
  }

  // Create the NT timer object. SynchronizationTimer (auto-reset) ensures
  // each firing is consumed exactly once by the registered wait.
  auto timer_oa = windows::internal_oa();
  NTSTATUS status = ::NtCreateTimer2(
      &slot->nt_timer, nullptr, &timer_oa,
      0, // SynchronizationTimer, no special attributes
      TIMER_ALL_ACCESS);

  if (!NT_SUCCESS(status)) {
    timer_manager::free_slot(static_cast<unsigned>(idx));
    return -EAGAIN;
  }

  // For SIGEV_SIGNAL and SIGEV_THREAD, watch the timer via the reactor.
  // The reactor's WCP fires when the SynchronizationTimer becomes signaled.
  slot->reactor_token = reactor::INVALID_TOKEN;
  if (notify != SIGEV_NONE) {
    slot->reactor_token = reactor::watch(
        slot->nt_timer, timer_manager::timer_reactor_cb, slot);
    if (!slot->reactor_token.valid()) {
      ::NtClose(slot->nt_timer);
      timer_manager::free_slot(static_cast<unsigned>(idx));
      return -EAGAIN;
    }
  }

  // Initialize slot fields.
  slot->clock_id = clockid;
  slot->sigev_notify = notify;
  slot->sigev_signo = signo;
  slot->sigev_value = sival;
  slot->sigev_notify_function = notify_fn;
  slot->interval_hns = 0;
  slot->next_fire_hns = 0;
  slot->overrun.store(0, cpp::MemoryOrder::RELAXED);

  // Extract thread stack size from sigev_notify_attributes if provided.
  slot->thread_stack_size = 0;
  if (notify == SIGEV_THREAD && sevp && sevp->sigev_notify_attributes)
    slot->thread_stack_size = sevp->sigev_notify_attributes->__stacksize;

  *timerid = timer_manager::index_to_timer(static_cast<unsigned>(idx));
  return 0;
}

intptr_t timer_settime(timer_t timerid, int flags,
                   const struct itimerspec *__restrict new_value,
                   struct itimerspec *__restrict old_value) {
  if (!new_value)
    return -EINVAL;

  int idx = timer_manager::timer_to_index(timerid);
  if (idx < 0)
    return -EINVAL;

  auto *slot = timer_manager::timer_pool.slot_for(static_cast<unsigned>(idx));
  if (!slot)
    return -EINVAL;

  int st = slot->state.load(cpp::MemoryOrder::ACQUIRE);
  if (st == timer_manager::SLOT_FREE)
    return -EINVAL;

  // If caller wants the previous value, query it first.
  if (old_value) {
    TIMER_BASIC_INFORMATION tbi;
    NTSTATUS qs = ::NtQueryTimer(slot->nt_timer, TimerBasicInformation, &tbi,
                                 sizeof(tbi), nullptr);
    if (NT_SUCCESS(qs) && st == timer_manager::SLOT_ARMED) {
      // RemainingTime is in 100ns. Convert to timespec.
      long long remaining = tbi.RemainingTime.QuadPart;
      // NT returns negative relative time for remaining.
      if (remaining < 0)
        remaining = -remaining;
      timer_manager::hns_to_timespec(remaining, old_value->it_value);
      timer_manager::hns_to_timespec(slot->interval_hns,
                                     old_value->it_interval);
    } else {
      // Timer was not armed.
      old_value->it_value.tv_sec = 0;
      old_value->it_value.tv_nsec = 0;
      old_value->it_interval.tv_sec = 0;
      old_value->it_interval.tv_nsec = 0;
    }
  }

  // Disarm if it_value is zero.
  long long value_hns = timer_manager::timespec_to_hns(new_value->it_value);
  if (value_hns == 0) {
    if (st == timer_manager::SLOT_ARMED) {
      ::NtCancelTimer2(slot->nt_timer, nullptr);
      slot->state.store(timer_manager::SLOT_ALLOCATED,
                        cpp::MemoryOrder::RELEASE);
    }
    slot->interval_hns = 0;
    slot->next_fire_hns = 0;
    slot->overrun.store(0, cpp::MemoryOrder::RELAXED);
    return 0;
  }

  // Cancel any existing arming before re-arming.
  if (st == timer_manager::SLOT_ARMED)
    ::NtCancelTimer2(slot->nt_timer, nullptr);

  // Compute period.
  long long interval_hns =
      timer_manager::timespec_to_hns(new_value->it_interval);
  slot->interval_hns = interval_hns;

  // Compute due time.
  LARGE_INTEGER due =
      timer_manager::compute_due_time(slot->clock_id, value_hns, flags);

  // Compute expected first firing time for overrun tracking.
  if (flags & TIMER_ABSTIME) {
    slot->next_fire_hns = value_hns;
  } else {
    slot->next_fire_hns = timer_manager::now_hns(slot->clock_id) + value_hns;
  }

  NTSTATUS status;
  if (timer_manager::is_alarm_clock(slot->clock_id)) {
    // ALARM clocks wake the system from S3 suspend. NtSetTimer (v1) is the
    // only NT API with ResumeTimer support. Period is milliseconds (LONG) —
    // adequate for wake-from-suspend use cases.
    LONG period_ms = 0;
    if (interval_hns > 0)
      period_ms = static_cast<LONG>(interval_hns / 10000LL);
    status = ::NtSetTimer(slot->nt_timer, &due, nullptr, nullptr,
                          /*ResumeTimer=*/TRUE, period_ms, nullptr);
  } else {
    LARGE_INTEGER period_li;
    LARGE_INTEGER *period_ptr = nullptr;
    if (interval_hns > 0) {
      period_li.QuadPart = interval_hns;
      period_ptr = &period_li;
    }
    T2_SET_PARAMETERS params{};
    params.Version = 0;
    params.NoWakeTolerance = 0;
    status = ::NtSetTimer2(slot->nt_timer, &due, period_ptr, &params);
  }

  if (!NT_SUCCESS(status))
    return -EINVAL;

  slot->overrun.store(0, cpp::MemoryOrder::RELAXED);
  slot->state.store(timer_manager::SLOT_ARMED, cpp::MemoryOrder::RELEASE);
  return 0;
}

intptr_t timer_gettime(timer_t timerid, struct itimerspec *curr_value) {
  if (!curr_value)
    return -EINVAL;

  int idx = timer_manager::timer_to_index(timerid);
  if (idx < 0)
    return -EINVAL;

  auto *slot = timer_manager::timer_pool.slot_for(static_cast<unsigned>(idx));
  if (!slot)
    return -EINVAL;

  int st = slot->state.load(cpp::MemoryOrder::ACQUIRE);
  if (st == timer_manager::SLOT_FREE)
    return -EINVAL;

  // Fill interval from the slot.
  timer_manager::hns_to_timespec(slot->interval_hns, curr_value->it_interval);

  if (st != timer_manager::SLOT_ARMED) {
    // Timer is not armed — remaining time is zero.
    curr_value->it_value.tv_sec = 0;
    curr_value->it_value.tv_nsec = 0;
    return 0;
  }

  // Query remaining time from kernel.
  TIMER_BASIC_INFORMATION tbi;
  NTSTATUS status = ::NtQueryTimer(slot->nt_timer, TimerBasicInformation, &tbi,
                                   sizeof(tbi), nullptr);
  if (!NT_SUCCESS(status))
    return -EINVAL;

  long long remaining = tbi.RemainingTime.QuadPart;
  if (remaining < 0)
    remaining = -remaining;

  // If timer already expired (remaining == 0 or signaled), report zero.
  if (tbi.TimerState || remaining == 0) {
    curr_value->it_value.tv_sec = 0;
    curr_value->it_value.tv_nsec = 0;
  } else {
    timer_manager::hns_to_timespec(remaining, curr_value->it_value);
  }

  return 0;
}

intptr_t timer_getoverrun(timer_t timerid) {
  int idx = timer_manager::timer_to_index(timerid);
  if (idx < 0)
    return -EINVAL;

  auto *slot = timer_manager::timer_pool.slot_for(static_cast<unsigned>(idx));
  if (!slot)
    return -EINVAL;

  if (slot->state.load(cpp::MemoryOrder::ACQUIRE) ==
      timer_manager::SLOT_FREE)
    return -EINVAL;

  int count = slot->overrun.load(cpp::MemoryOrder::ACQUIRE);
  return (count > DELAYTIMER_MAX) ? DELAYTIMER_MAX : count;
}

intptr_t timer_delete(timer_t timerid) {
  int idx = timer_manager::timer_to_index(timerid);
  if (idx < 0)
    return -EINVAL;

  auto *slot = timer_manager::timer_pool.slot_for(static_cast<unsigned>(idx));
  if (!slot)
    return -EINVAL;

  int st = slot->state.load(cpp::MemoryOrder::ACQUIRE);
  if (st == timer_manager::SLOT_FREE)
    return -EINVAL;

  // Unwatch the reactor registration — blocks until any in-flight callback
  // completes (reactor::unwatch spin-waits on the dispatching flag).
  if (slot->reactor_token.valid()) {
    reactor::unwatch(slot->reactor_token);
    slot->reactor_token = reactor::INVALID_TOKEN;
  }

  // Cancel and close the NT timer.
  ::NtCancelTimer2(slot->nt_timer, nullptr);
  ::NtClose(slot->nt_timer);
  slot->nt_timer = nullptr;

  timer_manager::free_slot(static_cast<unsigned>(idx));
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// ---------------------------------------------------------------------------
// Fork reinit — close stale NT timer handles and release all timer slots.
// Runs after interval timer reset and before reactor rebuild. The reactor's
// fork_reinit will rebuild the IOCP, so any stale WCPs from timer watches
// are already orphaned.
// ---------------------------------------------------------------------------
void LIBC_NAMESPACE::internal::timer_create_fork_reinit() {
  LIBC_NAMESPACE::timer_manager::fork_reinit();
}

// ---------------------------------------------------------------------------
// Exec teardown — cancel, unwatch, close, and free all live POSIX timers.
//
// POSIX: "Upon successful completion of one of the exec functions, timer
// IDs outstanding in the calling process shall be invalid, and all pending
// timers shall be canceled."
//
// Unlike fork (where NT handles are orphaned and just need nulling), exec
// runs in the same process — handles are live and must be properly closed.
// Called from exec Phase 5 after quiesce_threads (single-threaded).
// ---------------------------------------------------------------------------
void LIBC_NAMESPACE::internal::timer_create_exec_teardown() {
  using namespace LIBC_NAMESPACE;

  uint8_t init_state =
      timer_manager::pool_init_state.load(cpp::MemoryOrder::ACQUIRE);
  if (init_state != timer_manager::POOL_INIT_READY)
    return;

  timer_manager::timer_pool.for_each_live(
      [](unsigned idx, timer_manager::TimerSlot *slot, void *) {
        // Unwatch the reactor registration — blocks until any in-flight
        // callback completes.
        if (slot->reactor_token.valid()) {
          internal::reactor::unwatch(slot->reactor_token);
          slot->reactor_token = internal::reactor::INVALID_TOKEN;
        }

        // Cancel and close the NT timer handle.
        if (slot->nt_timer) {
          ::NtCancelTimer2(slot->nt_timer, nullptr);
          ::NtClose(slot->nt_timer);
          slot->nt_timer = nullptr;
        }

        slot->state.store(timer_manager::SLOT_FREE,
                          cpp::MemoryOrder::RELAXED);
        timer_manager::timer_pool.mark_dead(idx);
      },
      nullptr);

  timer_manager::timer_pool.fork_reinit(); // decommit empty chunks
}
