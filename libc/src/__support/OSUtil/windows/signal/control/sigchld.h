//===-- SIGCHLD generation (Layer 4) -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SIGCHLD generation and siginfo_t population for child state changes.
//
// Thin bridge over the wait-free Crystalline-backed payload subsystem at
// signal/payload/sig_payload.{h,cpp}. The payload subsystem holds the
// per-signum atomic latest-event pointer; this file just exposes the
// SIGCHLD-shaped writer/reader surface that child_table.cpp and the
// dispatch engine call.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_SIGCHLD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_SIGCHLD_H

#include "hdr/types/siginfo_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace process_control {

// Generate SIGCHLD for a child state change. Publishes a payload record
// via the wait-free Crystalline domain and pends SIGCHLD on the
// process-wide bitmap.
//
// code:     CLD_EXITED / CLD_KILLED / CLD_STOPPED / CLD_CONTINUED
// pid:      child PID
// status:   exit code (CLD_EXITED) or signal number (CLD_KILLED/STOPPED)
// utime_us: child cumulative user-mode CPU time at the event, microseconds
// stime_us: child cumulative kernel-mode CPU time at the event, microseconds
//
// Caller queries times via NtQueryInformationProcess(ProcessTimes) on the
// child's handle and converts FILETIME 100-ns units to microseconds.
// Pass 0/0 if the times are unavailable (handle revoked, query failed).
void deliver_sigchld(int code, int pid, int status,
                     long long utime_us, long long stime_us);

// Populate a siginfo_t from the latest SIGCHLD record via the payload
// subsystem's wait-free protected reader. Handler-context safe.
void populate_sigchld_info(siginfo_t *info);

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_SIGCHLD_H
