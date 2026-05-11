//===-- Rich signal payload subsystem ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Wait-free coherent multi-field publish for signals carrying richer payload
// than the PendingSet sidecar can express atomically.
//
// Backed by a CrystallineDomain — wait-free SMR (Safe Memory Reclamation)
// already in production for the memory subsystem (va_substrate, partition,
// slab_pool). One per-signum atomic "latest event" pointer per supported
// producer; writers exchange-and-retire, readers protect via
// CrystallineDomain::read().
//
// Producers in scope:
//   * SIGCHLD       — code, pid, status, utime_us, stime_us
//   * SIGEV_SIGNAL  — timerid, overrun, sival
//   * Cross-process kill via ALPC — sender pid/uid, sival
//
// Properties:
//   * Wait-free reader: bounded fast path + cross-thread helping fallback
//     (no spin past Crystalline's protocol). Safe in signal-handler context.
//   * Wait-free writer: per-thread retire batch, asynchronous reclamation.
//     No CAS contention on a shared slot, no spin, no timeout.
//   * Signal-safe: pure atomics, no locks, no syscalls in hot path.
//   * NtTerminateThread-resilient: no shared mutable record. A killed
//     writer leaks at most one pool slot until the cursor reclaims; a
//     killed reader's reservation is taken over by Crystalline's helping
//     protocol on the next retire — no global wedge in either direction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PAYLOAD_SIG_PAYLOAD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PAYLOAD_SIG_PAYLOAD_H

#include "hdr/types/siginfo_t.h"
#include "hdr/types/union_sigval.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace payload {

// One-shot subsystem bring-up. Builds the record pool freelist and
// registers the Crystalline domain. Must complete-happen-before any
// writer or reader call. Called from signal_subsystem_init() during
// Tier B; single-threaded at that point.
void init_payload_subsystem();

// Subsystem teardown — clears the latest-event pointers. The Crystalline
// domain's own fini hook (registered via init_registration) handles
// per-thread reclamation drains.
void fini_payload_subsystem();

// SIGCHLD writer. Called from drain threads in child_table.cpp callbacks.
// Pool exhaustion: the SIGCHLD pend bit is still set by the caller, so
// the receiver still wakes — only the rich payload is missing for that
// event. POSIX permits these fields to be best-effort.
//
// Times in microseconds (queried from NtQueryInformationProcess(ProcessTimes)
// on the child handle, FILETIME 100-ns ticks divided by 10).
void publish_sigchld(int code, int pid, int status, long long utime_us,
                     long long stime_us);

// SIGEV_SIGNAL timer expiry writer. Called from timer_manager.h's
// reactor callback path. Receiver sees si_code=SI_TIMER plus the timer's
// id, overrun count, and sigevent value.
void publish_timer_signal(int signum, int timerid, int overrun,
                          union sigval value);

// Cross-process kill writer. Called from ALPC transport's receive path
// after sender attestation. Replaces the torn-tuple-prone
// CrossProcessSender side-array.
void publish_cross_kill(int signum, int code, int sender_pid,
                        unsigned sender_uid, union sigval value);

// Reader. Populates `info` with the latest payload published for `signum`,
// if any. Touches no fields if no payload has been published for this
// signum (caller is responsible for base si_signo / si_code init).
//
// Signal-handler safe.
void populate_signal_payload(siginfo_t *info, int signum);

// Fork child reinit — the child starts with an empty SIGCHLD/timer/kill
// payload state (no pre-fork events apply). Clears all latest pointers.
// Called from signal_fork_reinit; the Crystalline domain's own fork hook
// handles per-thread retire batches.
void fork_reinit_payload_subsystem();

} // namespace payload
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_PAYLOAD_SIG_PAYLOAD_H
