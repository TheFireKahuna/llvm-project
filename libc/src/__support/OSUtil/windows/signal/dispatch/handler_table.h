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
// HandlerEntry is a lightweight adapter between the public struct sigaction
// and the internal handler table representation. Modeled after upstream
// Linux libc's KernelSigaction pattern — centralizes field manipulation,
// provides typed conversion operators, and encapsulates the default-reset
// logic that was previously spread across handler_table.cpp.
//
// Separated from the dispatch engine so that sigaction() and sigwait() can
// access handlers without pulling in the full dispatch machinery.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_HANDLER_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_HANDLER_TABLE_H

#include "hdr/signal_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/struct_sigaction.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// HandlerEntry — adapter between public struct sigaction and internal table
// ---------------------------------------------------------------------------
//
// The internal handler table stores struct sigaction directly (no kernel ABI
// mismatch on NT-POSIX — the entire signal system is userspace). HandlerEntry
// provides:
//
//   - Typed assignment from public struct sigaction (operator=)
//   - Typed conversion back to struct sigaction (operator struct sigaction)
//   - reset() to restore SIG_DFL state without field-by-field manual ops
//   - disposition() for fast-path classification (DFL / IGN / custom)
//
// This mirrors upstream's KernelSigaction adapter pattern: a single point of
// truth for how signal dispositions are stored and converted.

struct HandlerEntry {
  struct sigaction action;

  // Assign from a public struct sigaction. Preserves the SA_SIGINFO union
  // semantics: sa_sigaction and sa_handler occupy the same storage.
  LIBC_INLINE HandlerEntry &operator=(const struct sigaction &sa) {
    action.sa_flags = sa.sa_flags;
    action.sa_mask = sa.sa_mask;
    if (sa.sa_flags & SA_SIGINFO)
      action.sa_sigaction = sa.sa_sigaction;
    else
      action.sa_handler = sa.sa_handler;
    return *this;
  }

  // Convert back to the public struct sigaction.
  LIBC_INLINE operator struct sigaction() const {
    struct sigaction sa;
    sa.sa_flags = action.sa_flags;
    sa.sa_mask = action.sa_mask;
    if (action.sa_flags & SA_SIGINFO)
      sa.sa_sigaction = action.sa_sigaction;
    else
      sa.sa_handler = action.sa_handler;
    return sa;
  }

  // Reset to SIG_DFL. Zeroes all fields — equivalent to the default
  // disposition for any signal.
  LIBC_INLINE void reset() {
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    __builtin_memset(&action.sa_mask, 0, sizeof(action.sa_mask));
  }

  // Disposition classification for fast-path bitmask updates.
  enum class Disposition : uint8_t { Default, Ignored, Custom };

  LIBC_INLINE Disposition disposition() const {
    if (action.sa_handler == SIG_IGN)
      return Disposition::Ignored;
    if (action.sa_handler == SIG_DFL)
      return Disposition::Default;
    return Disposition::Custom;
  }

  // Direct accessors for common queries.
  LIBC_INLINE bool is_default() const {
    return action.sa_handler == SIG_DFL;
  }
  LIBC_INLINE bool is_ignored() const {
    return action.sa_handler == SIG_IGN;
  }
  LIBC_INLINE bool has_resethand() const {
    return (action.sa_flags & SA_RESETHAND) != 0;
  }
};

namespace handler_table {

// Read the current disposition for a signal. Handles SA_RESETHAND atomically:
// if the flag is set, resets the handler to SIG_DFL under the lock and clears
// the custom_handler_signals bit.
//
// This is the entry point for the dispatch engine before handler invocation.
[[nodiscard]] struct sigaction read_and_consume(int signum);

// Read the current disposition without consuming SA_RESETHAND.
// Used by sigaction() to report the current state.
[[nodiscard]] struct sigaction read(int signum);

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
