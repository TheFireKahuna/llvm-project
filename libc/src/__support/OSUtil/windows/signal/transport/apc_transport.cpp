//===-- APC transport (Layer 2b) — intra-process signal delivery -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Intra-process signal delivery via NtQueueApcThreadEx2 special user APCs.
// The APC callback receives the interrupted thread's CONTEXT as a hidden
// 4th argument from KiUserApcDispatcher (r9 on x64, x3 on ARM64), enabling
// correct ucontext_t construction for SA_SIGINFO handlers.
//
// Cross-process delivery (kill, sigqueue) is handled by ALPC transport
// (alpc_transport.cpp). This file contains ONLY intra-process paths.
//
// APC parameter encoding (fits in 3 PVOID = 24 bytes):
//   p1: signum (bits 0-7) | magic (bits 8-23) | uid (bits 32-63)
//   p2: si_code (bits 0-31) | sender_pid (bits 32-63)
//   p3: si_value.sival_ptr
//
// The magic cookie (0xA51C) in bits 8-23 rejects stray APCs (COM, thread
// pool, debugger, etc.).
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

// ---------------------------------------------------------------------------
// APC parameter encoding/decoding — with magic cookie
// ---------------------------------------------------------------------------
//
// Three PVOID values = 24 bytes on x86_64.
//
//   p1: signum (bits 0-7) | magic 0xA51C (bits 8-23) | si_uid (bits 32-63)
//   p2: si_code (bits 0-31) | sender_pid (bits 32-63)
//   p3: si_value.sival_ptr

namespace {

inline constexpr uint16_t APC_SIGNAL_MAGIC = 0xA51C;

struct ApcParams {
  PVOID p1, p2, p3;
};

ApcParams encode_signal(int signum, int si_code, pid_t sender_pid,
                        uid_t sender_uid, union sigval sval) {
  ApcParams p;
  p.p1 = reinterpret_cast<PVOID>(
      static_cast<uint64_t>(static_cast<uint8_t>(signum)) |
      (static_cast<uint64_t>(APC_SIGNAL_MAGIC) << 8) |
      (static_cast<uint64_t>(sender_uid) << 32));
  p.p2 = reinterpret_cast<PVOID>(
      static_cast<uint64_t>(static_cast<uint32_t>(si_code)) |
      (static_cast<uint64_t>(static_cast<uint32_t>(sender_pid)) << 32));
  p.p3 = sval.sival_ptr;
  return p;
}

bool decode_signal(uint64_t p1_val, PVOID p2, PVOID p3, int &signum,
                   int &si_code, pid_t &sender_pid, uid_t &sender_uid,
                   union sigval &sval) {
  // Validate magic cookie.
  uint16_t magic = static_cast<uint16_t>((p1_val >> 8) & 0xFFFF);
  if (magic != APC_SIGNAL_MAGIC)
    return false;

  signum = static_cast<int>(p1_val & 0xFF);
  sender_uid = static_cast<uid_t>(p1_val >> 32);
  uint64_t packed = reinterpret_cast<uint64_t>(p2);
  si_code = static_cast<int>(static_cast<uint32_t>(packed & 0xFFFFFFFF));
  sender_pid = static_cast<pid_t>(static_cast<uint32_t>(packed >> 32));
  sval.sival_ptr = p3;
  return true;
}

// Pend a signal to the process-wide set. Used when the receiving thread
// is blocking the signal or has no signal state.
void pend_to_process(int signum, int si_code, pid_t sender_pid,
                     uid_t sender_uid, union sigval sval) {
  if (signum >= SIGRTMIN && signum <= SIGRTMAX) {
    SigqueueEntry *entry = sigqueue_alloc();
    if (entry) {
      entry->si_signo = signum;
      entry->si_code = si_code;
      entry->value = sval;
      entry->pid = sender_pid;
      entry->uid = sender_uid;
      signal_pending::pend_rt(g_pcb.signal_dispatch.process_pending, entry);
    }
  } else {
    signal_pending::pend_standard(g_pcb.signal_dispatch.process_pending,
                                  signum);
  }

  signal_dispatch::trigger_any_thread();
}

// Pend a signal to a specific thread's pending set.
void pend_to_thread(ThreadSignalState *state, int signum, int si_code,
                    pid_t sender_pid, uid_t sender_uid,
                    union sigval sval) {
  if (signum >= SIGRTMIN && signum <= SIGRTMAX) {
    SigqueueEntry *entry = sigqueue_alloc();
    if (entry) {
      entry->si_signo = signum;
      entry->si_code = si_code;
      entry->value = sval;
      entry->pid = sender_pid;
      entry->uid = sender_uid;
      signal_pending::pend_rt(state->pending, entry);
    }
  } else {
    signal_pending::pend_standard(state->pending, signum);
  }
  signal_dispatch::trigger(state);
}

// Get the current process PID from the TEB (fast, no syscall).
pid_t get_current_pid() {
  pid_t pid;
#ifdef __x86_64__
  __asm__ __volatile__("movl %%gs:0x40, %0" : "=r"(pid));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %w0, [x18, #0x40]" : "=r"(pid));
#else
#error "Unsupported architecture"
#endif
  return pid;
}

// ---------------------------------------------------------------------------
// Intra-process APC callback — interrupted CONTEXT via hidden 4th argument
// ---------------------------------------------------------------------------
//
// KiUserApcDispatcher passes the interrupted thread's CONTEXT* as a hidden
// 4th argument to every APC routine (r9 on x64, x3 on ARM64). This is the
// same mechanism that RtlDispatchAPC uses internally for CALLBACK_DATA_CONTEXT
// wrapping — it's part of the KiUserApcDispatcher → APC routine calling
// convention, not a stack layout assumption.
//
// Verified by disassembly of KiUserApcDispatcher on Windows 11 24H2 (26200):
//   mov r9, rsp           ; r9 = CONTEXT base (RSP IS the CONTEXT)
//   ...                   ; decode routine, load args into rcx/rdx/r8
//   call cfg_helper       ; CFG helper explicitly saves/restores r9
//   jmp rax               ; tail-call to APC routine with r9 intact
//
// PPS_APC_ROUTINE only declares 3 parameters, but the 4th (CONTEXT*) is
// always present in the register. Declaring a 4th parameter captures it
// via the standard calling convention — no naked stubs or stack probing.
//
// The callback:
//   1. Validates the ContextFlags field of the CONTEXT.
//   2. Decodes signal parameters from p1, p2, p3 (magic-cookie validated).
//   3. Stores the CONTEXT pointer in state->interrupted_context.
//   4. Pends the signal and dispatches immediately.
//   5. Clears interrupted_context after dispatch.
//
// Async-signal-safe: all operations are lock-free atomics, TLS reads, and
// pointer dereferences. See SECURITY_CONSIDERATIONS.md §8.

} // close anonymous namespace

// ---------------------------------------------------------------------------
// APC entry point — receives interrupted CONTEXT as hidden 4th argument
// ---------------------------------------------------------------------------
//
// Declared with 4 parameters to match the actual KiUserApcDispatcher calling
// convention. PPS_APC_ROUTINE only declares 3, but the 4th (CONTEXT*) is
// always passed in r9 (x64) / x3 (ARM64). The reinterpret_cast at the call
// site is safe — the extra parameter is already in the register regardless.

void NTAPI intra_process_apc(PVOID p1_raw, PVOID p2, PVOID p3,
                             CONTEXT *interrupted) {
  uint64_t p1_val = reinterpret_cast<uint64_t>(p1_raw);

  int signum;
  int si_code;
  pid_t sender_pid;
  uid_t sender_uid;
  union sigval sval;

  // Validate magic cookie — reject stray APCs.
  if (!decode_signal(p1_val, p2, p3, signum, si_code, sender_pid, sender_uid,
                     sval))
    return;

  if (!is_valid_signal(signum))
    return;

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
    // Thread has no signal state. Pend to process-wide.
    pend_to_process(signum, si_code, sender_pid, sender_uid, sval);
    return;
  }

  // Store the interrupted CONTEXT pointer. Valid for the duration of this
  // APC callback frame — dispatch_pending() is called below within the
  // same frame, so the pointer remains live on KiUserApcDispatcher's stack.
  state->interrupted_context = interrupted;

  // Pend to this thread's pending set (thread-directed — always pend here,
  // even if blocked, per POSIX pthread_kill semantics).
  pend_to_thread(state, signum, si_code, sender_pid, sender_uid, sval);

  // Dispatch immediately from APC context. The dispatch engine's reentrancy
  // guard (DRAINING check) prevents nested dispatch if we interrupted an
  // ongoing dispatch. The CONTEXT pointer is valid because we're still in
  // the APC callback frame.
  signal_dispatch::dispatch_pending(state);

  // Clear the context pointer after dispatch. Signals delivered later
  // (deferred, unblocked) won't carry the original context — matching
  // POSIX behavior where pending signals delivered outside the original
  // interruption point don't have meaningful context.
  state->interrupted_context = nullptr;
}

// ---------------------------------------------------------------------------
// Intra-process delivery (pthread_kill)
// ---------------------------------------------------------------------------

namespace apc_transport {

intptr_t send_to_thread_local(ThreadLifecycle *target, int signum,
                              const siginfo_t *info);

intptr_t send_to_thread(DWORD target_tid, int signum, const siginfo_t *info) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  EpochGuard guard;
  ThreadLifecycle *target = guard.find(target_tid);
  if (!target || target->owner_tid.load(cpp::MemoryOrder::ACQUIRE) == 0)
    return -ESRCH;

  return send_to_thread_local(target, signum, info);
}

intptr_t send_to_thread_local(ThreadLifecycle *target, int signum,
                              const siginfo_t *info) {
  if (!is_valid_signal(signum))
    return -EINVAL;

  // Validate target has signal state BEFORE queuing APC.
  if (!target->signal)
    return -ESRCH;

  // Signal 0 is the null signal — validate only, don't deliver.
  if (signum == 0)
    return 0;

  // Encode signal parameters (with magic cookie in bits 8-23).
  int si_code = info ? info->si_code : SI_USER;
  pid_t sender_pid = info ? info->si_pid : get_current_pid();
  uid_t sender_uid = info ? info->si_uid : 0;
  union sigval sval = {};
  if (info)
    sval = info->si_value;

  ApcParams params = encode_signal(signum, si_code, sender_pid, sender_uid,
                                   sval);

  // Queue signal APC to the target thread.
  HANDLE h = registry_borrow_handle(target);
  if (!h)
    return -ESRCH;

  // intra_process_apc has 4 params (the 4th is the hidden CONTEXT* from
  // KiUserApcDispatcher). Cast through void* to avoid -Wcast-function-type
  // since PPS_APC_ROUTINE only declares 3 — the 4th is always passed
  // regardless of the typedef.
  auto apc_fn = reinterpret_cast<PPS_APC_ROUTINE>(
      reinterpret_cast<void *>(intra_process_apc));
  NTSTATUS status = windows::queue_signal_apc(
      h, apc_fn, params.p1, params.p2, params.p3);
  if (!NT_SUCCESS(status))
    return -ESRCH;

  // Alert the thread so it wakes from any blocking wait.
  alert_thread(target->owner_tid.load(cpp::MemoryOrder::RELAXED));

  return 0;
}

} // namespace apc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
