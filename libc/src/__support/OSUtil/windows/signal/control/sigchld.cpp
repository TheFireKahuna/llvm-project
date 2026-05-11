//===-- SIGCHLD generation (Layer 4) --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin bridge between the SIGCHLD generation call sites in child_table.cpp
// and the wait-free Crystalline-backed payload subsystem in
// signal/payload/. The payload subsystem holds the per-signum atomic
// "latest event" pointer and the Crystalline domain that gives readers a
// safe protected dereference; this file just publishes a SIGCHLD event
// and pends the bit.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/control/sigchld.h"

#include "hdr/signal_macros.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_engine.h"
#include "src/__support/OSUtil/windows/signal/payload/sig_payload.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace process_control {

void deliver_sigchld(int code, int pid, int status, long long utime_us,
                     long long stime_us) {
  // Publish the rich payload first. If the payload pool is exhausted the
  // publish is a no-op — the pend bit below still wakes the receiver, who
  // will see only si_signo + si_code (POSIX permits these fields to be
  // best-effort).
  payload::publish_sigchld(code, pid, status, utime_us, stime_us);

  // Pend SIGCHLD on the process-wide bitmap. The si_code sidecar carries
  // CLD_* for build_standard_siginfo's base population; the rich payload
  // (pid/status/utime/stime) comes from the Crystalline-protected record
  // that populate_signal_payload() reads.
  (void)signal_pending::pend_standard(g_pcb.signal_dispatch.process_pending,
                                      SIGCHLD, code);
  signal_dispatch::trigger_any_thread();
}

void populate_sigchld_info(siginfo_t *info) {
  info->si_signo = SIGCHLD;
  // The payload reader populates si_code, si_pid, si_status, si_utime,
  // si_stime from the latest published SIGCHLD record. If no event has
  // been published (or the pool was exhausted at publish time), the
  // remaining fields stay zero — POSIX-acceptable best-effort.
  payload::populate_signal_payload(info, SIGCHLD);
}

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
