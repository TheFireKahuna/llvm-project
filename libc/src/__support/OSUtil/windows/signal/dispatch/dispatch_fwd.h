//===-- Dispatch engine forward declarations ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for Layer 3 dispatch engine functions. Used by Layer 2
// transports that need to trigger dispatch after pending a signal.
//
// The actual implementations live in dispatch/dispatch_engine.cpp (Layer 3,
// Step 3 of the implementation order). This header allows Layers 1 and 2
// to compile and link independently — the linker resolves these symbols
// when Layer 3 is built.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_DISPATCH_FWD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_DISPATCH_FWD_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Forward declaration — full definition in signal_types.h.
struct ThreadSignalState;

// Forward declaration — defined in signal.h / signal_state.cpp.
// Needed by transports to get thread signal state.
ThreadSignalState *get_thread_state_noinit();

namespace signal_dispatch {

// Trigger dispatch on a specific thread. Called by transports after pend().
// Transitions the thread's dispatch state to TRIGGERED (or no-ops if
// already TRIGGERED or DRAINING).
void trigger(ThreadSignalState *state);

// Trigger dispatch on any suitable thread. Used for process-directed signals
// (console, cross-process APC pended to process-wide set).
// Tries preferred_thread first, falls back to registry walk.
void trigger_any_thread();

// Drain all pending signals on the calling thread. Called by APC/VEH
// transports to deliver synchronous signals immediately. The dispatch
// engine's reentrancy guard (DRAINING check) prevents nested dispatch.
void dispatch_pending(ThreadSignalState *state);

} // namespace signal_dispatch
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_DISPATCH_FWD_H
