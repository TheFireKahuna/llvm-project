//===-- Windows implementation of abort ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/abort.h"
#include "hdr/signal_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigprocmask.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, abort, ()) {
  // POSIX: unblock SIGABRT so the handler runs even if the caller blocked it.
  sigset_t mask;
  mask.__signals[0] = 1UL << (SIGABRT - 1);
  LIBC_NAMESPACE::sigprocmask(SIG_UNBLOCK, &mask, nullptr);

  // First raise: runs the user's SIGABRT handler (if installed via sigaction).
  LIBC_NAMESPACE::raise(SIGABRT);

  // Handler returned — reset to default and raise again. SIG_DFL for SIGABRT
  // calls execute_default_action → TerminateProcess(128 + SIGABRT).
  struct sigaction action = {};
  action.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGABRT, &action, nullptr);
  LIBC_NAMESPACE::raise(SIGABRT);

  // Unreachable: SIG_DFL for SIGABRT terminates the process. Safety net in
  // case signal delivery is somehow broken.
  __builtin_trap();
}

} // namespace LIBC_NAMESPACE_DECL
