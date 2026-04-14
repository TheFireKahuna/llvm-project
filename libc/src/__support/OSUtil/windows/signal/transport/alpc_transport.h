//===-- ALPC transport (Layer 2c) — cross-process signal delivery -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cross-process signal delivery via ALPC (Advanced Local Procedure Call).
// Replaces the PE-export-walking APC mechanism with kernel-native IPC.
//
// Architecture:
//   - Each NT-POSIX process creates an ALPC port inside a private namespace
//     bound to the current user's SID + medium integrity level.
//   - Port name: "NtPosixSig-{pid}-{create_time_hex}" inside the namespace.
//   - The port is registered with the process-wide reactor; the drain thread
//     receives connection requests via IOCP completion and pends signals.
//   - Senders deliver one signal per NtAlpcConnectPortEx handshake: the
//     connection request carries the signal payload and sender create_time.
//   - After the receiver validates and pends the signal, both sides tear the
//     connection down immediately. No long-lived signal comm ports are kept.
//
// Security properties:
//   1. Unforgeable sender identity (kernel-filled PORT_MESSAGE.ClientId)
//   2. DACL-enforced port access (kernel evaluates at connect time)
//   3. Private namespace prevents port enumeration
//   4. PID-reuse hardening via sender create_time revalidation
//   5. Three-tier permission checks (DACL → sender-side → receiver-side)
//
// Replaces: export_resolve.h/cpp, cross-process path in apc_transport.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_ALPC_TRANSPORT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_ALPC_TRANSPORT_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_types.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// Signal message format (ALPC payload)
// ---------------------------------------------------------------------------
//
// 32 bytes of payload after PORT_MESSAGE header. Still well within ALPC's
// small-message threshold — no shared memory view or handle transfer needed.
//
// Fields NOT in the message (kernel-provided):
//   si_pid: PORT_MESSAGE.ClientId.UniqueProcess
//   si_uid: derived from the sender token after ClientId + create_time
//           validation on the receiver side

inline constexpr uint32_t SIGNAL_MAGIC = 0x50534947;   // "PSIG" in LE
inline constexpr uint16_t SIGNAL_VERSION = 1;

struct SignalMessage {
  PORT_MESSAGE header;       // Kernel-filled: sender PID/TID, message length.
  uint32_t magic;            // SIGNAL_MAGIC — protocol identifier.
  uint16_t version;          // SIGNAL_VERSION.
  uint8_t signum;            // 1..NSIG-1.
  uint8_t flags;             // Reserved (alignment).
  int32_t si_code;           // SI_USER, SI_QUEUE, etc.
  uint32_t reserved;         // Reserved for future flags / padding.
  uint64_t sender_create_time; // Sender create_time, revalidated by receiver.
  uint64_t value;            // sigval union (sival_ptr or sival_int).
};

static_assert(sizeof(SignalMessage) - sizeof(PORT_MESSAGE) == 32,
              "SignalMessage payload must be exactly 32 bytes");

namespace alpc_transport {

// ---------------------------------------------------------------------------
// Subsystem lifecycle
// ---------------------------------------------------------------------------

// Initialize the ALPC transport: create private namespace, create port,
// build security descriptor, start listener thread. Called from
// signal_startup_init() (Phase 7 of __libc_dll_init()).
//
// Returns 0 on success, -1 on failure. On failure, cross-process signal
// delivery is disabled but intra-process operations still work.
int init();

// Shutdown: stop listener thread, close port, close namespace.
// Called from signal_subsystem_fini().
void fini();

// Fork child reinit: close inherited port/connections, create child's
// own port, start new listener. Called from signal_fork_reinit().
void fork_reinit();

// Pre-exec cleanup: close port and stop listener. Called before
// NtCreateUserProcess in execve/posix_spawn.
void pre_exec_cleanup();

// ---------------------------------------------------------------------------
// Cross-process delivery (kill, sigqueue)
// ---------------------------------------------------------------------------

// Send a signal to another process via ALPC.
//
// Returns:
//   0       on success (message sent, signal pended in target)
//   -ESRCH  target not found, not NT-POSIX, or PID reused
//   -EPERM  insufficient privileges (DACL or token check)
//   -EINVAL invalid signal number
intptr_t send_to_process(pid_t pid, int signum, const siginfo_t *info);

// ---------------------------------------------------------------------------
// Permission check (exposed for kill() sender-side EPERM)
// ---------------------------------------------------------------------------

// Check whether the current process has permission to signal target_pid.
// Returns 0 on success, -ESRCH if not found, -EPERM if denied.
int check_send_permission(pid_t target_pid);

} // namespace alpc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_ALPC_TRANSPORT_H
