//===-- Internal poll declaration --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::poll() — implements Linux syscall semantics
// (returns non-negative ready count on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_POLL_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_POLL_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/nfds_t.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/struct_timespec.h"
#include "src/__support/macros/config.h"

struct pollfd; // forward declare

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns number of ready fds (non-negative) or -errno on failure.
intptr_t poll(struct pollfd *fds, nfds_t nfds, int timeout);

// ppoll: poll with nanosecond-precision timeout and atomic signal mask.
// If sigmask is non-null, the signal mask is atomically swapped for the
// duration of the wait (save → install → alertable wait → restore),
// following the sigsuspend pattern for race-free signal delivery.
intptr_t ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo,
               const sigset_t *sigmask);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_POLL_OPS_H
