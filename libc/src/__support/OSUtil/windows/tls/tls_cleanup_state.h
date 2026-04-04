//===-- Canonical TLS cleanup state for Windows ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lightweight process-wide TLS cleanup registry types used directly in the
// PCB. This keeps the storage model typed without dragging in the cleanup
// execution helpers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_TLS_CLEANUP_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_TLS_CLEANUP_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Callbacks are libc-internal: registered by libc subsystems and invoked
// only from tls_cleanup_run_all(), which runs under the libc default ABI.
// No NT or PE-loader code ever sees these pointers — the MS-ABI boundary
// is pinned one frame up, at the .CRT$XLC entry point. Do not add
// LIBC_MSABI here: it is both unnecessary and forbidden in the trailing
// position of a type alias (see __llvm-libc-common.h).
using TlsCleanupFn_FN = void(void *);
using TlsCleanupFn = TlsCleanupFn_FN *;

inline constexpr unsigned TLS_CLEANUP_MAX_SLOTS = 16;

// Cleanup phases. tls_cleanup_run_all walks in DESCENDING phase order
// (high phase = late init = early teardown), then within a phase in
// reverse-registration order. This is load-bearing: lifecycle (Phase 4)
// retires the dying thread's lifecycle into the Crystalline scratch
// region, which must still be live; the scratch arena's release at
// Phase 1 must therefore run AFTER Phase 4 has finished retiring.
//
// Phase numbers mirror the libc subsystem startup phases so a single
// constant denotes both "init phase N" and "teardown phase N runs
// before init-phase-(N-1) teardown".
enum : uint8_t {
  // Tier-A fundamentals: ThreadScratch arena, slab pools that own the
  // VA the higher tiers retire into. Cleaned up LAST.
  kTlsCleanupPhaseSubstrate = 1,
  // Phase 2 — Posix allocator state, etc.
  kTlsCleanupPhaseAllocator = 2,
  // Phase 3 — wait-slot pool. Wait slots back the futex parking lot
  // every higher-phase teardown may use; teardown after Phase 4.
  kTlsCleanupPhaseWaitSlot = 3,
  // Phase 4 — thread lifecycle, signal state, IoRing, bucket-entry
  // slab. Cleaned up FIRST so retires reach the live Crystalline domain.
  kTlsCleanupPhaseLifecycle = 4,
};

struct TlsCleanupEntry {
  DWORD tls_index;
  TlsCleanupFn callback;
  // Phase byte; tls_cleanup_run_all walks in descending phase order so
  // higher-phase teardowns observe a live lower-phase substrate.
  uint8_t phase;
  uint8_t _pad[3];
};

struct TlsCleanupState {
  TlsCleanupEntry entries[TLS_CLEANUP_MAX_SLOTS];
  cpp::Atomic<unsigned> count;
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_TLS_CLEANUP_STATE_H
