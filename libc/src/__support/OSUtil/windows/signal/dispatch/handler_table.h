//===-- Signal handler table (Layer 3) ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-global sigaction table. Wraps g_pcb.signal_handler.handlers[] with
// RawMutex-protected access and SA_RESETHAND atomicity.
//
// Separated from the dispatch engine so that sigaction() and sigwait() can
// access handlers without pulling in the full dispatch machinery.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_HANDLER_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_HANDLER_TABLE_H

#include "hdr/types/struct_sigaction.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace handler_table {

// Read the current disposition for a signal. Handles SA_RESETHAND atomically:
// if the flag is set, resets the handler to SIG_DFL under the lock and clears
// the custom_handler_signals bit.
//
// This is the entry point for the dispatch engine before handler invocation.
struct sigaction read_and_consume(int signum);

// Read the current disposition without consuming SA_RESETHAND.
// Used by sigaction() to report the current state.
struct sigaction read(int signum);

// Write a new disposition. Used by sigaction(). Updates the handler table,
// ignored_signals, and custom_handler_signals bitmasks atomically.
// Returns the previous disposition.
struct sigaction write(int signum, const struct sigaction *new_act);

// Deferred re-fault reset from VEH. Resets handler to SIG_DFL under the lock.
// VEH stores the signal number instead of writing sa_handler lock-free to
// avoid a TOCTOU race with concurrent sigaction().
void deferred_reset(int signum);

} // namespace handler_table
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_HANDLER_TABLE_H
