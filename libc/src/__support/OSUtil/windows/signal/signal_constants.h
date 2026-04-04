//===-- Signal bitmask constants and helpers --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Leaf header: no dependencies on signal_types.h or pending_storage.h.
// Both of those include this to break the circular dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_SIGNAL_CONSTANTS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_SIGNAL_CONSTANTS_H

#include "hdr/signal_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/sigset_t.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// sigset_t ↔ uint64_t conversion
// ---------------------------------------------------------------------------

LIBC_INLINE uint64_t sigset_to_bits(const sigset_t &set) {
  return static_cast<uint64_t>(set.__signals[0]) |
         (static_cast<uint64_t>(set.__signals[1]) << 32);
}

LIBC_INLINE void bits_to_sigset(uint64_t bits, sigset_t *set) {
  set->__signals[0] = static_cast<unsigned long>(bits);
  set->__signals[1] = static_cast<unsigned long>(bits >> 32);
}

// ---------------------------------------------------------------------------
// Bitmask constants
// ---------------------------------------------------------------------------

// Bitmask of stop signals whose pending bits are cleared on SIGCONT,
// and vice versa (POSIX mutual cancellation).
inline constexpr uint64_t STOP_SIGNALS_MASK =
    (1ULL << (SIGTSTP - 1)) | (1ULL << (SIGTTIN - 1)) |
    (1ULL << (SIGTTOU - 1));
inline constexpr uint64_t SIGCONT_MASK = 1ULL << (SIGCONT - 1);

// Mask covering all valid signal bits in pending_signals / process_pending.
// Signals are numbered 1..NSIG-1, occupying bits 0..NSIG-2 (i.e., 0..62).
inline constexpr uint64_t SIGNAL_BITS_MASK = (1ULL << 63) - 1;

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_SIGNAL_CONSTANTS_H
