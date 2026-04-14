//===-- Internal select declaration -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declaration for internal::select() — implements Linux syscall
// semantics (returns non-negative ready count on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SELECT_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SELECT_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/struct_timespec.h"
#include "include/llvm-libc-types/fd_set.h"
#include "include/llvm-libc-types/struct_timeval.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns number of ready fds (non-negative) or -errno on failure.
intptr_t select(int nfds, fd_set *__restrict read_set,
                fd_set *__restrict write_set, fd_set *__restrict error_set,
                struct timeval *__restrict timeout);

// pselect: select with nanosecond-precision timeout and atomic signal mask.
// Converts fd_sets to pollfds and delegates to ppoll for readiness
// (including atomic signal masking). Does NOT modify the timeout.
intptr_t pselect(int nfds, fd_set *__restrict read_set,
                 fd_set *__restrict write_set, fd_set *__restrict error_set,
                 const struct timespec *__restrict timeout,
                 const sigset_t *__restrict sigmask);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_SELECT_OPS_H
