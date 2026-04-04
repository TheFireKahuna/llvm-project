//===-- APC transport (Layer 2b) — intra-process signal delivery -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Intra-process signal delivery via NT user APCs with CALLBACK_DATA_CONTEXT.
//
// Two entry points:
//
//   send_to_thread()       — by TID (pthread_kill)
//   send_to_thread_local() — by lifecycle pointer (fast path)
//
// Cross-process delivery (kill, sigqueue) is handled by ALPC transport
// (alpc_transport.h). This file is intra-process only — no exported symbols,
// no PE export resolution, no cross-process APC.
//
// CALLBACK_DATA_CONTEXT (Windows 11+):
//   APC callback receives APC_CALLBACK_DATA_CONTEXT* as first argument,
//   providing the interrupted thread's full CONTEXT record. This enables
//   correct ucontext_t for SA_SIGINFO handlers.
//
// APC parameter encoding (fits in 3 PVOID = 24 bytes):
//   p1: signum (bits 0-7) | magic (bits 8-23) | uid (bits 32-63)
//   p2: si_code (bits 0-31) | sender_pid (bits 32-63)
//   p3: si_value.sival_ptr
//
// Magic cookie 0xA51C in bits 8-23 of p1 prevents stray APCs (COM, thread
// pool, debugger) from being misinterpreted as signal deliveries.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_APC_TRANSPORT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_APC_TRANSPORT_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// Forward declaration — full definition in thread_lifecycle.h.
struct ThreadLifecycle;

namespace signal_state {
namespace apc_transport {

// -------------------------------------------------------------------------
// Intra-process delivery (pthread_kill)
// -------------------------------------------------------------------------

// Send signal to a specific thread within the current process by TID.
//
// Validates target has signal state BEFORE queuing the APC. Returns:
//   0       on success (APC queued, thread alerted)
//   -ESRCH  target thread not found or has no signal state
//   -EINVAL invalid signal number
intptr_t send_to_thread(DWORD target_tid, int signum, const siginfo_t *info);

// Send signal to a specific thread using ThreadLifecycle pointer directly.
//
// PRECONDITION: The caller must keep `target` alive for the duration
// of the call — either via the Crystalline reservation that
// registry_find_if / registry_resolve hands out, or via POSIX-trust on
// a `pthread_t` whose `platform_data` was deref'd to reach `target`.
// Currently only called from send_to_thread() which holds a
// reservation through registry_find_if.
//
// Returns:
//   0       on success
//   -ESRCH  target has no signal state
//   -EINVAL invalid signal number
intptr_t send_to_thread_local(ThreadLifecycle *target, int signum,
                          const siginfo_t *info);

} // namespace apc_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_APC_TRANSPORT_H
