//===-- ALPC transport (Layer 2c) — signal-delivery consumer ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cross-process signal delivery, now implemented as a thin consumer of the
// generic ALPC bus (see ipc/alpc_bus.h). This file still owns the signal
// semantics — message schema, permission model, pending-queue integration —
// but no longer runs its own port, namespace, or listener. At init it
// registers a handler for OP_SIGNAL_DELIVER; at send time it calls
// alpc_bus::request().
//
// Security properties (unchanged from the original transport):
//   1. Unforgeable sender identity — kernel-filled PORT_MESSAGE.ClientId
//   2. DACL-enforced port access — evaluated by the kernel at connect time
//   3. Private namespace prevents port enumeration
//   4. PID-reuse hardening — sender create_time revalidated on receive
//   5. Three-tier permission model (DACL → sender-side → receiver-side)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_ALPC_TRANSPORT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_ALPC_TRANSPORT_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// Signal payload carried on the bus (opcode OP_SIGNAL_DELIVER)
// ---------------------------------------------------------------------------
//
// Everything the receiver needs that isn't kernel-attested is in this
// struct. `si_pid` is recovered from PORT_MESSAGE.ClientId (kernel-filled);
// `si_uid` is resolved from the sender token by the bus dispatcher before
// the handler runs.
//
// Keeping the payload explicit (as opposed to "struct siginfo_t on the
// wire") means we can evolve siginfo_t's ABI on either side without
// breaking cross-process signal delivery.

struct SignalPayload {
  uint32_t signum;        // 1..NSIG-1, validated on receive.
  int32_t  si_code;       // SI_USER, SI_QUEUE, etc.
  uint64_t value;         // sigval raw bits (sival_ptr or sival_int).
};

static_assert(sizeof(SignalPayload) == 16,
              "SignalPayload layout is part of the wire format");

namespace alpc_transport {

// ---------------------------------------------------------------------------
// Subsystem lifecycle
// ---------------------------------------------------------------------------

// Register the OP_SIGNAL_DELIVER handler with the bus. Must be called
// before `alpc_bus::init()` (or at the same phase) so inbound signals
// aren't rejected with STATUS_NOT_SUPPORTED during the init window.
// Returns 0 on success, -1 on duplicate registration or bad state.
int init();

// Currently a no-op — the bus owns every port/namespace handle that
// needs tearing down. Kept so signal_state can keep its subsystem-fini
// call shape symmetric with init().
void fini();

// Same story: the bus handles port rebuild on fork; the signal consumer
// has no per-process state to reset here. Left in the interface so
// signal_state::signal_fork_reinit() continues to compile.
void fork_reinit();

// ---------------------------------------------------------------------------
// Cross-process delivery (kill, sigqueue)
// ---------------------------------------------------------------------------

// Send a signal to another process via the bus.
//
// Returns:
//   0       on success (message sent, signal pended in target)
//   -ESRCH  target not found, not NT-POSIX, or PID reused
//   -EPERM  insufficient privileges (DACL or token check)
//   -EINVAL invalid signal number
intptr_t send_to_process(pid_t pid, int signum, const siginfo_t *info);

// ---------------------------------------------------------------------------
// Permission check (sender-side EPERM for kill())
// ---------------------------------------------------------------------------

// Returns 0 on success, -ESRCH if the target does not exist, -EPERM if
// the POSIX permission model forbids the send. Delegates to the bus's
// shared helper — same policy applies anywhere we need peer-authz.
int check_send_permission(pid_t target_pid);

} // namespace alpc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_ALPC_TRANSPORT_H
