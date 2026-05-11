//===-- APC transport (Layer 2b) — intra-process signal delivery -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Intra-process signal delivery via NtQueueApcThreadEx2 special user APCs.
//
// Design: the APC is *notification only* — it does not carry signal content.
// The sender pends the full signal record (si_code, pid, uid, sval) into the
// target thread's PendingSet BEFORE queuing the APC. The APC callback's
// responsibilities shrink to: (1) validate the APC is actually ours via a
// magic cookie, (2) capture the interrupted CONTEXT for SA_SIGINFO, and
// (3) call dispatch_pending() to drain whatever is pending.
//
// Rationale (see signal drop audit 2026-04-20):
//   - If the kernel drops the APC during thread termination, the pended
//     signal is still in state->pending. Thread-exit cleanup drains
//     state->pending into process_pending so the signal is not lost.
//   - RT sigqueue_alloc() failures can now return -EAGAIN synchronously
//     to the sender (POSIX-correct for sigqueue(3)) instead of being
//     silently dropped inside the APC callback after send acknowledged.
//
// APC payload: magic cookie only, in the low 16 bits of p1. p2, p3 unused.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/transport/apc_transport.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/apc.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_engine.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/pending/sigqueue_pool.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

namespace {

// Magic cookie distinguishing our APCs from stray APCs queued by COM, the
// Windows thread pool, debugger injection, etc. Placed in bits 0-15 of p1.
inline constexpr uint16_t APC_SIGNAL_MAGIC = 0xA51C;

// Pack the magic cookie (and signum for diagnostics) into the first APC
// parameter. Signum is redundant for dispatch (the PendingSet carries it)
// but included for crash-dump forensics.
inline PVOID encode_apc_p1(int signum) {
  return reinterpret_cast<PVOID>(
      static_cast<uint64_t>(APC_SIGNAL_MAGIC) |
      (static_cast<uint64_t>(static_cast<uint8_t>(signum)) << 16));
}

inline bool validate_apc_p1(uint64_t p1_val) {
  return static_cast<uint16_t>(p1_val & 0xFFFF) == APC_SIGNAL_MAGIC;
}

// Recover the sender's signum from the encoded p1 cookie. Used to bind
// the captured CONTEXT to its specific signum so dispatch_pending only
// feeds the original-fault-or-sender CONTEXT to *that* signal's handler,
// not to other signals that happen to be pending in the same drain pass.
inline int decode_apc_p1_signum(uint64_t p1_val) {
  return static_cast<int>((p1_val >> 16) & 0xFF);
}

// Pend a signal record into a PendingSet, allocating an RT entry if needed.
// Returns true on success, false only on RT sigqueue_alloc() failure (OOM).
// Standard signals never fail here — the bitmap CAS is wait-free.
[[nodiscard]] bool pend_into(PendingSet &ps, int signum, int si_code,
                             pid_t sender_pid, uid_t sender_uid,
                             union sigval sval) {
  if (signum >= SIGRTMIN && signum <= SIGRTMAX) {
    SigqueueEntry *entry = sigqueue_alloc();
    if (!entry)
      return false;
    entry->si_signo = signum;
    entry->si_code = si_code;
    entry->value = sval;
    entry->pid = sender_pid;
    entry->uid = sender_uid;
    signal_pending::pend_rt(ps, entry);
    return true;
  }
  (void)signal_pending::pend_standard(ps, signum, si_code);
  return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// APC entry point — receives interrupted CONTEXT as hidden 4th argument
// ---------------------------------------------------------------------------
//
// KiUserApcDispatcher passes the interrupted thread's CONTEXT* as a hidden
// 4th argument to every APC routine (r9 on x64, x3 on ARM64). PPS_APC_ROUTINE
// only declares 3 params; the 4th is always present in the register.
// Verified by disassembly of KiUserApcDispatcher on Windows 11 24H2 (26200):
//
//   mov r9, rsp           ; r9 = CONTEXT base (RSP IS the CONTEXT)
//   ...                   ; decode routine, load args into rcx/rdx/r8
//   call cfg_helper       ; CFG helper explicitly saves/restores r9
//   jmp rax               ; tail-call to APC routine with r9 intact
//
// The callback:
//   1. Validates the magic cookie in p1 — reject stray APCs.
//   2. Validates the ContextFlags field of the CONTEXT.
//   3. Stores the CONTEXT pointer in state->interrupted_context.
//   4. Calls dispatch_pending — drains whatever the sender pended into
//      state->pending.
//   5. Clears interrupted_context after dispatch.

NTAPI void intra_process_apc(PVOID p1_raw, PVOID /*p2*/, PVOID /*p3*/,
                             CONTEXT *interrupted) {
  uint64_t p1_val = reinterpret_cast<uint64_t>(p1_raw);

  if (!validate_apc_p1(p1_val))
    return; // Not our APC — stray from COM/thread pool/debugger.

  // Validate the CONTEXT by checking ContextFlags for the architecture flag.
#if defined(__x86_64__)
  constexpr DWORD kContextArchFlag = 0x00100000; // CONTEXT_AMD64
#elif defined(__aarch64__)
  constexpr DWORD kContextArchFlag = 0x00400000; // CONTEXT_ARM64
#else
#error "Unsupported architecture for CONTEXT validation"
#endif
  if (interrupted &&
      (interrupted->ContextFlags & kContextArchFlag) != kContextArchFlag)
    interrupted = nullptr;

  ThreadSignalState *state = get_thread_state_noinit();
  if (!state) {
    // Target thread has no signal state. The sender should have rejected
    // this at send_to_thread_local (line `if (!target->signal) return
    // -ESRCH`), so reaching this branch means the state was torn down
    // between send-time validation and APC delivery. Nothing to dispatch.
    return;
  }

  // Capture the interrupted CONTEXT for SA_SIGINFO handlers. Valid for
  // the duration of this APC frame — dispatch_pending runs synchronously
  // below, so the pointer remains live on KiUserApcDispatcher's stack.
  //
  // Bind it to the sender's signum (recovered from the APC cookie). The
  // dispatch drain loop processes all pending standard signals lowest-
  // first; without the binding, a co-pending signal would inherit this
  // CONTEXT and ship a wrong-context ucontext_t to its SA_SIGINFO handler.
  state->interrupted_context = interrupted;
  state->interrupted_signum = decode_apc_p1_signum(p1_val);

  // The sender already pended the signal into state->pending and set the
  // notify::SIGNAL bit via trigger(). dispatch_pending drains everything
  // pending and applies handlers.
  signal_dispatch::dispatch_pending(state);

  // Signals pended but not delivered this pass (blocked, etc.) remain in
  // state->pending for a later dispatch boundary. Context won't carry
  // forward — matches POSIX: deferred delivery has no meaningful context.
  state->interrupted_context = nullptr;
  state->interrupted_signum = 0;
}

// ---------------------------------------------------------------------------
// Intra-process delivery (pthread_kill)
// ---------------------------------------------------------------------------

namespace apc_transport {

intptr_t send_to_thread(DWORD target_tid, int signum, const siginfo_t *info) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  // O(N) walk over the iter list to find the lifecycle whose immutable
  // tid matches. The registry's primary index is task_id, but the
  // syscall surface (SYS_tgkill / SYS_tkill) hands us a raw NT TID.
  // The walk is Crystalline-protected; the returned lifecycle is
  // pinned for the call frame.
  ThreadLifecycle *target = registry_find_if(
      [&](ThreadLifecycle *lc) -> bool { return lc->tid == target_tid; });
  if (!target)
    return -ESRCH;

  return send_to_thread_local(target, signum, info);
}

intptr_t send_to_thread_local(ThreadLifecycle *target, int signum,
                              const siginfo_t *info) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  // Validate target has signal state BEFORE pending anything. A foreign
  // thread (or a thread whose signal state has been torn down) cannot
  // receive thread-directed signals — POSIX pthread_kill to such a thread
  // is ESRCH, not a silent reroute to some other thread.
  // Single ACQUIRE load — torn-read guard against a concurrent
  // deregister_thread_state flipping target->signal to nullptr.
  ThreadSignalState *state =
      target->signal.load(cpp::MemoryOrder::ACQUIRE);
  if (!state)
    return -ESRCH;

  // Signal 0 is the null signal — validate only, don't deliver.
  if (signum == 0)
    return 0;

  // Decode siginfo (nullable — synthesize defaults for SI_USER).
  int si_code = info ? info->si_code : SI_USER;
  pid_t sender_pid =
      info ? info->si_pid : static_cast<pid_t>(NtCurrentProcessId());
  uid_t sender_uid = info ? info->si_uid : 0;
  union sigval sval = {};
  if (info)
    sval = info->si_value;

  // Acquire the target's exit gate. The ACQ_REL fetch_add gives us three
  // things at once: (a) a SEQ_CST-equivalent total ordering against the
  // cleanup thread's fetch_or on the same location, (b) an atomic view of
  // the DRAINING bit that was visible just before our add, and (c) a
  // declaration of in-flight intent that cleanup must observe as a non-
  // zero count before it will drain.
  //
  // If DRAINING was already set, the exiting thread has closed the gate
  // and we must not pend into state->pending — our entry would never be
  // drained. Release the in-flight count and redirect to process_pending
  // instead. POSIX pthread_kill to a thread that has entered exit returns
  // ESRCH; we still route the signal to the surviving process so a
  // process-wide handler (e.g., SIGTERM) runs.
  uint32_t gate_prev =
      state->exit_gate.fetch_add(ThreadSignalState::EXIT_GATE_IN_FLIGHT_STEP,
                                 cpp::MemoryOrder::ACQ_REL);
  if (gate_prev & ThreadSignalState::EXIT_GATE_DRAINING) {
    state->exit_gate.fetch_sub(ThreadSignalState::EXIT_GATE_IN_FLIGHT_STEP,
                               cpp::MemoryOrder::RELEASE);
    if (!pend_into(g_pcb.signal_dispatch.process_pending, signum, si_code,
                   sender_pid, sender_uid, sval))
      return -EAGAIN;
    signal_dispatch::trigger_any_thread();
    return -ESRCH;
  }

  // Gate held. We have producer-exclusive access to state->pending for
  // this operation — cleanup cannot complete its drain until we release.
  if (!pend_into(state->pending, signum, si_code, sender_pid, sender_uid,
                 sval)) {
    state->exit_gate.fetch_sub(ThreadSignalState::EXIT_GATE_IN_FLIGHT_STEP,
                               cpp::MemoryOrder::RELEASE);
    return -EAGAIN; // RT sigqueue pool OOM; POSIX-correct for sigqueue(3).
  }

  // Set notify::SIGNAL on the target's notify_word so any owner parked in
  // wait_for_change wakes and sees the pending bit. Safe to do while the
  // gate is held — trigger only touches notify_word, not state->pending.
  signal_dispatch::trigger(state);

  // Release the gate. From here, cleanup may start draining at any time,
  // and our pend is guaranteed to be seen by it (our fetch_sub is RELEASE;
  // cleanup's subsequent observation of in_flight==0 is ACQUIRE).
  state->exit_gate.fetch_sub(ThreadSignalState::EXIT_GATE_IN_FLIGHT_STEP,
                             cpp::MemoryOrder::RELEASE);

  // Probe target liveness for ESRCH semantics. The gate already
  // guarantees no signal drop regardless of the race outcome:
  //   - If the borrowed thread handle is still in place, cleanup has
  //     not yet released it; our pend sits in state->pending and the
  //     APC we're about to queue will drive dispatch.
  //   - If the handle has been released, cleanup has already drained
  //     state->pending to process_pending (strictly sequenced after
  //     cleanup's in-flight==0 observation, and our RELEASE fetch_sub
  //     is ordered before that). The signal is safely in
  //     process_pending and a surviving thread will dispatch it.
  //
  // POSIX pthread_kill semantics: we return ESRCH when the target
  // thread is gone at the moment of delivery. The signal itself is
  // not lost.
  HANDLE h = registry_borrow_handle(target);
  if (!h)
    return -ESRCH;
  DWORD live_tid = target->tid;

  // Queue the notification APC and alert the thread. `h` was borrowed
  // above; intra_process_apc takes 4 params (the 4th is the hidden
  // CONTEXT* from KiUserApcDispatcher). Cast through void* to avoid
  // -Wcast-function-type since PPS_APC_ROUTINE only declares 3 — the
  // 4th is always passed regardless of the typedef.
  auto apc_fn = reinterpret_cast<PPS_APC_ROUTINE>(
      reinterpret_cast<void *>(intra_process_apc));
  NTSTATUS status = windows::queue_signal_apc(
      h, apc_fn, encode_apc_p1(signum), nullptr, nullptr);
  if (!NT_SUCCESS(status)) {
    // Kernel rejected the APC (thread terminating, handle closed, etc.).
    // The signal itself is still routed correctly via the gate + cleanup
    // drain. Surface ESRCH so the caller knows the notification path
    // failed on this thread.
    return -ESRCH;
  }

  alert_thread(live_tid);
  return 0;
}

} // namespace apc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
