//===-- SIGCHLD generation (Layer 4) --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/control/sigchld.h"

#include "hdr/signal_macros.h"
#include "hdr/types/siginfo_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_engine.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace process_control {

void deliver_sigchld(int code, int pid, int status) {
  // Single atomic store — no torn tuples. Standard signals are not queued,
  // so only the latest child state change survives (POSIX).
  g_pcb.signal_sigchld.packed.store(pack_sigchld(code, pid, status),
                                    cpp::MemoryOrder::RELEASE);

  // Pend SIGCHLD as a process-directed signal via Layer 1 API.
  signal_pending::pend_standard(g_pcb.signal_dispatch.process_pending,
                                SIGCHLD);
  signal_dispatch::trigger_any_thread();
}

void populate_sigchld_info(siginfo_t *info) {
  uint64_t packed =
      g_pcb.signal_sigchld.packed.load(cpp::MemoryOrder::ACQUIRE);

  int code, pid, status;
  unpack_sigchld(packed, code, pid, status);

  info->si_signo = SIGCHLD;
  info->si_code = code;
  info->si_pid = pid;
  info->si_status = status;
}

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
