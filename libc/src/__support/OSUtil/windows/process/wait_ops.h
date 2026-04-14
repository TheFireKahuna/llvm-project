//===-- Internal wait family declarations ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: wait functions that implement POSIX wait
// family semantics (return 0/-errno or positive-pid/-errno).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_WAIT_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_WAIT_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/id_t.h"
#include "hdr/types/idtype_t.h"
#include "hdr/types/pid_t.h"
#include "src/__support/macros/config.h"

#include <signal.h>

struct rusage;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Core waitid implementation. Returns 0 on success, -errno on failure.
intptr_t waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);

/// waitpid conversion layer. Returns positive pid on success, -errno on failure.
/// Translates waitpid(pid, ...) semantics to waitid then converts siginfo_t
/// back to a traditional wait status word.
intptr_t waitpid(pid_t pid, int *wstatus, int options);

/// wait4 conversion layer. Same as waitpid but also fills rusage from the
/// waited child's exit snapshot.
intptr_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_WAIT_OPS_H
