//===-- Internal setitimer/getitimer engine implementation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements POSIX setitimer(ITIMER_REAL) using an NT timer object watched
// by the process-wide reactor. The reactor's drain thread delivers SIGALRM
// when the timer fires.
//
// Timer state (lock, handle, deadline, frequency, interval) lives in the PCB
// as g_pcb.itimer. The reactor token is implementation-private and stays
// file-local.
//
// Returns 0 on success, -errno on failure. No libc_errno references.
//
//===----------------------------------------------------------------------===//

#include "itimer_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/struct_itimerval.h"
#include "include/llvm-libc-macros/sys-time-macros.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Reactor token — implementation-private to this file. ReactorToken is
// {ReactorSlot*, uint32_t} (16 bytes), used only for watch/rearm within
// this TU. Not observable process state, so it stays file-local.
static reactor::ReactorToken g_reactor_token{reactor::INVALID_TOKEN};

// Convert timeval to 100ns units.
static LONGLONG timeval_to_100ns(const struct timeval *tv) {
  return static_cast<LONGLONG>(tv->tv_sec) * 10000000LL +
         static_cast<LONGLONG>(tv->tv_usec) * 10LL;
}

static bool timeval_is_zero(const struct timeval *tv) {
  return tv->tv_sec == 0 && tv->tv_usec == 0;
}

// Lazily cache QPC frequency. Caller must hold itimer lock (exclusive).
static void init_qpc_freq(ItimerState &state) {
  if (state.qpc_freq == 0) {
    LARGE_INTEGER freq;
    ::RtlQueryPerformanceFrequency(&freq);
    state.qpc_freq = freq.QuadPart;
  }
}

// Fill an itimerval with the remaining time and current interval.
// Caller must hold itimer lock (shared or exclusive).
static void fill_remaining(ItimerState &state, struct itimerval *out) {
  out->it_interval = state.current.it_interval;

  if (!state.timer || state.deadline_qpc == 0) {
    out->it_value.tv_sec = 0;
    out->it_value.tv_usec = 0;
    return;
  }

  LARGE_INTEGER now;
  ::RtlQueryPerformanceCounter(&now);
  LONGLONG remaining_ticks = state.deadline_qpc - now.QuadPart;
  if (remaining_ticks <= 0) {
    out->it_value.tv_sec = 0;
    out->it_value.tv_usec = 0;
    return;
  }

  // Convert QPC ticks -> timeval.
  LONGLONG remaining_us = (remaining_ticks * 1000000LL) / state.qpc_freq;
  out->it_value.tv_sec = static_cast<time_t>(remaining_us / 1000000LL);
  out->it_value.tv_usec =
      static_cast<suseconds_t>(remaining_us % 1000000LL);
}

// Reactor callback — fires on the drain thread when the timer is signaled.
//
// POSIX: SIGALRM from setitimer is process-directed — any thread with
// SIGALRM unblocked may receive it. deliver_process_signal implements
// this: preferred thread first, then registry walk.
//
// Note on disarm race: if setitimer({0,0}) disarms while this callback is
// in-flight, the lock-protected re-read of current.it_interval sees the
// disarmed state ({0,0}) and zeroes deadline_qpc — the states converge.
static void real_timer_reactor_cb(void * /*context*/, NTSTATUS /*status*/,
                                  ULONG_PTR /*information*/) {
  signal_state::deliver_process_signal(SIGALRM);

  // Update deadline for getitimer(). The NT timer handles periodic re-firing;
  // we track the next deadline with QPC for remaining-time reporting.
  auto &timer = g_pcb.itimer;
  timer.lock.lock();
  if (!timeval_is_zero(&timer.current.it_interval)) {
    // Convert interval from 100ns units to QPC ticks.
    LONGLONG interval_100ns = timeval_to_100ns(&timer.current.it_interval);
    LONGLONG interval_qpc = (interval_100ns * timer.qpc_freq) / 10000000LL;
    timer.deadline_qpc += interval_qpc;
  } else {
    timer.deadline_qpc = 0;
  }
  timer.lock.unlock();

  // Re-arm the reactor watch for the next firing. Harmless for one-shot
  // timers — the SynchronizationTimer won't signal again until re-armed
  // via NtSetTimer2.
  reactor::rearm(g_reactor_token);
}

static int ensure_real_timer_registered(ItimerState &state) {
  if (state.timer)
    return 0;

  windows::ScopedNtHandle timer;
  auto timer_oa = windows::internal_oa();
  NTSTATUS status =
      ::NtCreateTimer2(timer.put(), nullptr, &timer_oa,
                       0, // SynchronizationTimer, no special attributes
                       TIMER_ALL_ACCESS);
  if (!NT_SUCCESS(status))
    return -EAGAIN;

  // Watch the timer via the process-wide reactor. The WCP fires when the
  // SynchronizationTimer becomes signaled.
  reactor::ReactorToken token =
      reactor::watch(timer.get(), real_timer_reactor_cb, nullptr);
  if (!token.valid())
    return -EAGAIN;

  state.timer = timer.release();
  g_reactor_token = token;
  return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// setitimer engine
// ---------------------------------------------------------------------------
intptr_t setitimer(int which, const struct itimerval *new_value,
               struct itimerval *old_value) {
  if (which != ITIMER_REAL)
    return -ENOSYS;

  if (!new_value)
    return -EINVAL;

  // Validate microseconds range.
  if (new_value->it_value.tv_usec < 0 ||
      new_value->it_value.tv_usec >= 1000000 ||
      new_value->it_interval.tv_usec < 0 ||
      new_value->it_interval.tv_usec >= 1000000)
    return -EINVAL;

  auto &timer = g_pcb.itimer;
  timer.lock.lock();
  init_qpc_freq(timer);

  // Return previous value if requested.
  if (old_value)
    fill_remaining(timer, old_value);

  // Provision the timer/wait pair before disturbing any existing arming.
  if (!timeval_is_zero(&new_value->it_value)) {
    int init_result = ensure_real_timer_registered(timer);
    if (init_result != 0) {
      timer.lock.unlock();
      return init_result;
    }
  }

  // Disarm existing timer.
  if (timer.timer)
    ::NtCancelTimer2(timer.timer, nullptr);

  timer.deadline_qpc = 0;

  // Store the new configuration.
  timer.current = *new_value;

  if (!timeval_is_zero(&new_value->it_value)) {
    LARGE_INTEGER due;
    due.QuadPart = -timeval_to_100ns(&new_value->it_value);

    LARGE_INTEGER period_li;
    LARGE_INTEGER *period_ptr = nullptr;
    if (!timeval_is_zero(&new_value->it_interval)) {
      period_li.QuadPart = timeval_to_100ns(&new_value->it_interval);
      period_ptr = &period_li;
    }

    T2_SET_PARAMETERS params{};
    params.Version = 0;
    params.NoWakeTolerance = 0;

    NTSTATUS status =
        ::NtSetTimer2(timer.timer, &due, period_ptr, &params);
    if (!NT_SUCCESS(status)) {
      timer.current = {};
      timer.lock.unlock();
      return -EINVAL;
    }

    // Track deadline for getitimer().
    LARGE_INTEGER now;
    ::RtlQueryPerformanceCounter(&now);
    LONGLONG due_ticks =
        (timeval_to_100ns(&new_value->it_value) * timer.qpc_freq) /
        10000000LL;
    timer.deadline_qpc = now.QuadPart + due_ticks;
  }

  timer.lock.unlock();
  return 0;
}

// ---------------------------------------------------------------------------
// getitimer engine
// ---------------------------------------------------------------------------
intptr_t getitimer(int which, struct itimerval *curr_value) {
  if (which != ITIMER_REAL)
    return -ENOSYS;

  if (!curr_value)
    return -EINVAL;

  auto &timer = g_pcb.itimer;
  timer.lock.lock();
  fill_remaining(timer, curr_value);
  timer.lock.unlock();
  return 0;
}

// ---------------------------------------------------------------------------
// Fork reinit
// ---------------------------------------------------------------------------
static void setitimer_fork_reinit_impl() {
  auto &timer = g_pcb.itimer;
  timer.lock.reset_for_fork();
  // The parent's timer was created with internal_oa() (non-inheritable),
  // so it doesn't exist in the child's handle table. Just null it.
  timer.timer = nullptr;
  timer.deadline_qpc = 0;
  timer.current = {};
  g_reactor_token = reactor::INVALID_TOKEN; // Invalidated by reactor fork_reinit.
}

// Exec teardown: cancel + close live NT timer handle, unwatch reactor.
// Unlike fork (handle orphaned), exec keeps the handle live — must close.
static void setitimer_exec_teardown_impl() {
  auto &timer = g_pcb.itimer;

  // Single-threaded after quiesce — lock reset safe.
  timer.lock.reset_for_fork();

  // Unwatch reactor registration first (blocks until any in-flight
  // SIGALRM callback completes).
  if (g_reactor_token.valid()) {
    reactor::unwatch(g_reactor_token);
    g_reactor_token = reactor::INVALID_TOKEN;
  }

  // Cancel and close the NT timer handle.
  if (timer.timer) {
    ::NtCancelTimer2(timer.timer, nullptr);
    ::NtClose(timer.timer);
    timer.timer = nullptr;
  }

  timer.deadline_qpc = 0;
  timer.current = {};
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Reset lock and discard stale timer state. The reactor fork_reinit
// already invalidated the slot; the child recreates the timer and
// reactor watch on the next setitimer call.
void LIBC_NAMESPACE::internal::setitimer_fork_reinit() {
  LIBC_NAMESPACE::internal::setitimer_fork_reinit_impl();
}

// Exec teardown — cancel + close the ITIMER_REAL NT timer and reactor
// registration. POSIX: interval timers are not preserved across exec.
void LIBC_NAMESPACE::internal::setitimer_exec_teardown() {
  LIBC_NAMESPACE::internal::setitimer_exec_teardown_impl();
}

LIBC_REGISTER_FORK_REINIT(setitimer,
                          ::LIBC_NAMESPACE::internal::kForkPrioSetitimer,
                          &::LIBC_NAMESPACE::internal::setitimer_fork_reinit)
