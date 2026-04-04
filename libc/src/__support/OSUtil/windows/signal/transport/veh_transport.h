//===-- VEH transport (Layer 2a) ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Vectored Exception Handler transport for hardware exceptions. Statically
// registered into the unified VEH dispatch table via .libcveh — see the
// LIBC_REGISTER_VEH_FILTER record at the bottom of veh_transport.cpp.
//
// The handler is installed in Tier A (before Phase 0c PCB writes) and is
// guarded at runtime by g_pcb.signal_handler.{custom,ignored} — pre-Tier-B
// firing returns EXCEPTION_CONTINUE_SEARCH because no signal handler can
// be installed before Tier B's signal subsystem comes up.
//
// Maps Windows exception codes to POSIX signal numbers, builds siginfo_t,
// and pends via Layer 1. The three-stage filter (severity gate → language
// runtime skip → allowlist) is preserved from the current implementation.
//
// Re-fault detection: (last_fault_pc, last_fault_code) tracking prevents
// infinite re-delivery when a handler doesn't fix the cause. The pair is
// reset at dispatch boundary, not at pend time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_VEH_TRANSPORT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_VEH_TRANSPORT_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace veh_transport {

// VEH filter callback. Statically registered in the .libcveh table; the
// master VEH dispatcher invokes it for exceptions matching VEH_ALL_SIGNAL.
//
// Returns EXCEPTION_CONTINUE_EXECUTION if handled (signal pended, dispatch
// triggered), EXCEPTION_CONTINUE_SEARCH otherwise.
LONG handle_exception(EXCEPTION_POINTERS *ep);

} // namespace veh_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_VEH_TRANSPORT_H
