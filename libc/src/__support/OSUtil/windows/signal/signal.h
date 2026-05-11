//===-- Windows signal subsystem public API ------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// External API for the Windows signal subsystem. These are the functions called
// by code outside libc/src/signal/windows/ (pthread, File, unistd, time,
// threads, startup, etc.).
//
// Internal APIs are in the layer headers:
//   pending/pending_storage.h     — Layer 1 (pend/drain)
//   transport/apc_transport.h     — Layer 2b (cross-thread/process APC)
//   dispatch/dispatch_engine.h    — Layer 3 (dispatch state machine)
//   control/process_control.h     — Layer 4 (stop/continue/default actions)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_H
#define LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/stack_t.h"
#include "hdr/types/struct_sigaction.h"
#include "hdr/types/ucontext_t.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/common.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// Subsystem lifecycle
// ---------------------------------------------------------------------------

int signal_subsystem_init();
void signal_subsystem_fini();
void signal_fork_reinit();

void init_signal_state();
void fini_signal_state();
ThreadSignalState *get_thread_state();
ThreadSignalState *get_thread_state_noinit();
void register_thread_state(ThreadSignalState *state);
void deregister_thread_state(ThreadSignalState *state);

// ---------------------------------------------------------------------------
// Signal query / control
// ---------------------------------------------------------------------------

bool is_signal_blocked(int signum);
void generate_standard_signal_for_current_thread(int signum);
bool should_restart_syscall();

// ---------------------------------------------------------------------------
// Signal delivery (external entry points)
// ---------------------------------------------------------------------------

intptr_t deliver_signal_to_thread(DWORD tid, int signum);
intptr_t deliver_process_signal(int signum);

// ---------------------------------------------------------------------------
// Kernel functions — implement Linux syscall semantics in userspace.
// Return -errno on failure, 0 on success.
//
// Two overload tiers:
//   - Typed: used by syscall wrappers and direct callers (preferred).
//   - void*: used by the generic syscall dispatch in syscall.h, where all
//     arguments arrive as intptr_t and type headers are not available.
//     These are thin casts that delegate to the typed versions.
// ---------------------------------------------------------------------------

// Typed overloads (preferred call path).
[[nodiscard]] intptr_t rt_sigprocmask(int how, const sigset_t *set,
                                      sigset_t *oldset);
[[nodiscard]] intptr_t rt_sigaction(int sig,
                                    const struct sigaction *__restrict act,
                                    struct sigaction *__restrict oldact);
[[nodiscard]] intptr_t sigaltstack(const stack_t *ss, stack_t *oss);
[[nodiscard]] intptr_t kill(intptr_t pid, int sig);

// void* overloads for generic syscall dispatch (syscall.h).
[[nodiscard]] intptr_t rt_sigprocmask(int how, const void *set, void *oldset);
[[nodiscard]] intptr_t rt_sigaction(int sig, const void *act, void *oldact);
[[nodiscard]] intptr_t sigaltstack(const void *ss, void *oss);

// ---------------------------------------------------------------------------
// SIGCHLD metadata
// ---------------------------------------------------------------------------

void deliver_sigchld(int code, int pid, int status, long long utime_us,
                     long long stime_us);
void populate_sigchld_info(siginfo_t *info);

// ---------------------------------------------------------------------------
// Inherited child state (posix_spawn protocol)
// ---------------------------------------------------------------------------

void init_inherited_child_state();
void fini_inherited_child_state();
void notify_parent_state_change(int state);

// ---------------------------------------------------------------------------
// Context conversion (used by VEH transport and dispatch engine)
// ---------------------------------------------------------------------------

void context_win32_to_ucontext(const CONTEXT *win_ctx, ucontext_t *uc);

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_H
