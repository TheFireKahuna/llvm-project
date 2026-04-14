//===-- SIGCHLD generation (Layer 4) -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SIGCHLD generation and siginfo_t population for child state changes.
//
// Uses a compact 64-bit atomic packing: [63:60] code, [59:32] pid,
// [31:0] status. Standard signals are not queued (POSIX), so only the
// latest child state change survives. A single atomic store/load pair
// provides torn-read-free access without locking.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_SIGCHLD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_SIGCHLD_H

#include "hdr/types/siginfo_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace process_control {

// ---------------------------------------------------------------------------
// SIGCHLD metadata packing
// ---------------------------------------------------------------------------
//
// Single uint64_t carries all siginfo fields needed for SIGCHLD:
//   [63:60] si_code  — CLD_EXITED, CLD_KILLED, CLD_STOPPED, etc. (4 bits)
//   [59:32] si_pid   — child PID (28 bits, sufficient for Windows PIDs)
//   [31:0]  si_status — exit code or signal number (32 bits)
//
// This packing allows a single atomic store from the wait path and a
// single atomic load from the dispatch path. No locking needed.

LIBC_INLINE uint64_t pack_sigchld(int code, int pid, int status) {
  return (static_cast<uint64_t>(code & 0xF) << 60) |
         (static_cast<uint64_t>(pid & 0x0FFFFFFF) << 32) |
         (static_cast<uint64_t>(static_cast<unsigned>(status)));
}

LIBC_INLINE void unpack_sigchld(uint64_t packed, int &code, int &pid,
                                int &status) {
  code = static_cast<int>((packed >> 60) & 0xF);
  pid = static_cast<int>((packed >> 32) & 0x0FFFFFFF);
  status = static_cast<int>(packed & 0xFFFFFFFF);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Generate SIGCHLD for a child state change. Atomically stores the packed
// metadata and pends SIGCHLD as a process-directed signal.
//
// code: CLD_EXITED, CLD_KILLED, CLD_STOPPED, CLD_CONTINUED, etc.
// pid:  child PID
// status: exit code (CLD_EXITED) or signal number (CLD_KILLED/STOPPED)
void deliver_sigchld(int code, int pid, int status);

// Populate a siginfo_t from the packed SIGCHLD metadata.
// Called by the dispatch engine when delivering SIGCHLD to a handler.
void populate_sigchld_info(siginfo_t *info);

} // namespace process_control
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_CONTROL_SIGCHLD_H
