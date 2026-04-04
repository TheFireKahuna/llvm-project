//===-- Signal engine forward declarations -------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal signal engine functions — return long (0 on success, -errno on
// failure, or signal number for sigtimedwait). Entry points in
// src/signal/windows/ are thin wrappers that translate to POSIX return
// conventions (libc_errno + -1).
//
// The signal_state:: functions (rt_sigaction, rt_sigprocmask, sigaltstack,
// kill) are declared in signal/signal.h. This header declares only the
// internal:: namespace functions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_SIGNAL_SYSCALLS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_SIGNAL_SYSCALLS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/struct_timespec.h"
#include "hdr/types/union_sigval.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t raise(int signum);
intptr_t sigpending(sigset_t *set);
intptr_t sigqueue(pid_t pid, int sig, const union sigval value);
// pthread_sigqueue(3) backend — RT signal with payload to a specific NT
// thread within the current process.
intptr_t sigqueue_thread(pid_t tid, int sig, const union sigval value);
intptr_t sigsuspend(const sigset_t *mask);
intptr_t sigtimedwait(const sigset_t *__restrict set, siginfo_t *__restrict info,
                  const struct timespec *__restrict timeout);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_SIGNAL_SYSCALLS_H
