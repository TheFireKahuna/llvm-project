//===-- ALPC transport — signal delivery on top of the ALPC bus -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin consumer of ipc/alpc_bus. This file owns the signal-specific slice:
//
//   - Outbound:  kill() / sigqueue() → send_to_process() →
//                alpc_bus::request(OP_SIGNAL_DELIVER, SignalPayload).
//
//   - Inbound:   alpc_bus dispatcher → on_signal_deliver() →
//                pend_cross_process_signal() into the process-pending set.
//
// All port / namespace / DACL / reactor plumbing now lives in alpc_bus. The
// security properties are unchanged because the bus preserves the original
// three-tier model verbatim.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/transport/alpc_transport.h"

#include "src/__support/OSUtil/windows/ipc/alpc_bus.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_fwd.h"
#include "src/__support/OSUtil/windows/signal/payload/sig_payload.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

namespace {

// Route a validated inbound signal into the process-wide pending set.
// Returns STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES on RT
// sigqueue pool exhaustion. The status flows back through the ALPC reply
// to the sender so sigqueue(3) callers see EAGAIN instead of a silent
// drop (see signal drop audit 2026-04-20).
int32_t pend_cross_process_signal(int signum, int si_code, pid_t sender_pid,
                                  uid_t sender_uid, uint64_t si_value_raw) {
  union sigval sval;
  sval.sival_ptr = reinterpret_cast<void *>(si_value_raw);

  if (signum >= SIGRTMIN && signum <= SIGRTMAX) {
    SigqueueEntry *entry = sigqueue_alloc();
    if (!entry)
      return STATUS_INSUFFICIENT_RESOURCES;
    entry->si_signo = signum;
    entry->si_code = si_code;
    entry->value = sval;
    entry->pid = sender_pid;
    entry->uid = sender_uid;
    signal_pending::pend_rt(g_pcb.signal_dispatch.process_pending, entry);
  } else {
    // Publish sender identity (pid, uid, sival) into the wait-free
    // payload subsystem. The receiver's build_standard_siginfo path
    // pulls these via populate_signal_payload alongside si_code from
    // the PendingSet sidecar — coherent multi-field publish, no
    // shared mutable side-array (the previous CrossProcessSender
    // process_sender[] write here had a torn-tuple race with
    // concurrent same-signum senders).
    payload::publish_cross_kill(signum, si_code,
                                static_cast<int>(sender_pid),
                                static_cast<unsigned>(sender_uid), sval);
    (void)signal_pending::pend_standard(g_pcb.signal_dispatch.process_pending,
                                        signum, si_code);
  }
  signal_dispatch::trigger_any_thread();
  return STATUS_SUCCESS;
}

// Bus handler for OP_SIGNAL_DELIVER. Sender attestation (PID + create_time
// + UID) has already been done by the bus before we're called, so all we
// owe is payload validation and signal-side side effects.
int32_t on_signal_deliver(const internal::alpc_bus::RequestView &req,
                          const internal::alpc_bus::SenderInfo &sender,
                          const internal::alpc_bus::ReplyBuffer &reply) {
  if (req.size < sizeof(SignalPayload))
    return STATUS_INVALID_PARAMETER;

  const SignalPayload *p = static_cast<const SignalPayload *>(req.data);
  if (!is_valid_signal(static_cast<int>(p->signum)))
    return STATUS_INVALID_PARAMETER;

  int32_t st =
      pend_cross_process_signal(static_cast<int>(p->signum), p->si_code,
                                sender.pid, sender.uid, p->value);

  // No reply payload — bus header carries the status (success or OOM).
  if (reply.size_out)
    *reply.size_out = 0;
  return st;
}

} // anonymous namespace

namespace alpc_transport {

int init() {
  return internal::alpc_bus::register_handler(
      internal::alpc_bus::OP_SIGNAL_DELIVER, on_signal_deliver);
}

void fini() {
  // Bus owns every handle; nothing subsystem-specific to release here.
}

void fork_reinit() {
  // Handler registration is process-lifetime and survives fork; the bus
  // re-creates the port and re-registers with the reactor on its own.
}

intptr_t send_to_process(pid_t pid, int signum, const siginfo_t *info) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  // Signal 0 is an existence check — short-circuit without touching the
  // bus so the permission model matches POSIX even when the bus is down.
  if (signum == 0) {
    int pc = internal::alpc_bus::peer::check_signal_send_permission(pid);
    if (pc == -3 /* -ESRCH */) return -ESRCH;
    if (pc == -1 /* -EPERM */) return -EPERM;
    return 0;
  }

  if (!internal::alpc_bus::is_up()) {
    // Bus is down (init failed at startup or transport torn down). Do not
    // claim the target doesn't exist — that masks a libc-side failure as a
    // POSIX "no such process" and leaves callers with no way to distinguish
    // a live target from a dead one. Probe existence; return -EAGAIN if the
    // process is alive so callers can retry or fall back, -ESRCH only when
    // the process truly doesn't exist.
    windows::ScopedNtHandle probe;
    NTSTATUS st = ::NtOpenProcessById(
        probe.put(), PROCESS_QUERY_LIMITED_INFORMATION, pid);
    if (NT_SUCCESS(st))
      return -EAGAIN;
    return (st == STATUS_ACCESS_DENIED) ? -EPERM : -ESRCH;
  }

  // Tier 2 (sender-side) permission check so kill() returns EPERM
  // synchronously instead of routing through the bus and racing on ACL.
  int perm = check_send_permission(pid);
  if (perm != 0)
    return perm;

  SignalPayload payload{};
  payload.signum = static_cast<uint32_t>(signum);
  payload.si_code = info ? info->si_code : SI_USER;
  payload.value =
      info ? reinterpret_cast<uint64_t>(info->si_value.sival_ptr) : 0;

  constexpr int64_t TIMEOUT_500MS = -5000000LL; // 100-ns units, negative = relative.

  int32_t st = internal::alpc_bus::request(
      pid, internal::alpc_bus::OP_SIGNAL_DELIVER, &payload, sizeof(payload),
      /*reply_buf=*/nullptr, /*reply_cap=*/0, /*reply_len=*/nullptr,
      TIMEOUT_500MS);

  if (st == STATUS_SUCCESS)
    return 0;
  if (st == STATUS_ACCESS_DENIED)
    return -EPERM;
  // Receiver-side sigqueue pool exhaustion. POSIX sigqueue(3) returns
  // EAGAIN when the system cannot queue an RT signal; the handler at
  // on_signal_deliver propagates STATUS_INSUFFICIENT_RESOURCES over the
  // bus reply so the sender can surface it rather than silently drop.
  if (st == static_cast<int32_t>(STATUS_INSUFFICIENT_RESOURCES))
    return -EAGAIN;
  return -ESRCH;
}

int check_send_permission(pid_t target_pid) {
  int rc = internal::alpc_bus::peer::check_signal_send_permission(target_pid);
  if (rc == 0) return 0;
  if (rc == -3) return -ESRCH; // bus returns -ESRCH as -3 (avoid errno.h coupling)
  return -EPERM;
}

} // namespace alpc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
