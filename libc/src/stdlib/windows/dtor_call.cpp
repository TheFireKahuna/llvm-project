//===-- dtor_call.cpp - Fault-guarded destructor invocation ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per Itanium ABI 3.3.5: if a destructor throws during __cxa_finalize or
// __cxa_thread_finalize, std::terminate() must be called.
//
// C++ exceptions: The Itanium personality (__gxx_personality_seh0) handles
// this natively — an uncaught C++ exception propagates to _URC_END_OF_STACK
// and the runtime calls std::terminate(). No guard needed.
//
// Hardware exceptions (AV in unloaded DLL, stack overflow in foreign code):
// These bypass the C++ personality entirely. Without a guard they become an
// unhandled exception that can hang the process — especially with loader lock
// held during atexit finalization. The VEH fault guard catches all hardware
// faults (excluding debugger traps) and forces abort().
//
// CLR/COM/RPC exceptions have codes outside the VEH bitmask table and pass
// through to their own SEH personalities automatically.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

void invoke_exit_destructor(void (*dtor)(void *), void *obj) {
  windows::FaultGuard guard;
  if (windows::fault_guard_enter(&guard, windows::FAULT_GUARD_DTOR)) {
    // Hardware fault during destructor — process is in unknown state.
    // Use NtTerminateProcess directly instead of abort() to avoid
    // re-entering the signal subsystem (SIGABRT dispatch) or libc
    // teardown logic that may itself fault in a corrupted process.
    // 0x40000015 = STATUS_FATAL_APP_EXIT (same code abort() produces).
    ::NtTerminateProcess(NtCurrentProcess(), 0x40000015);
    __builtin_unreachable();
  }
  dtor(obj);
  windows::fault_guard_leave(&guard);
}

} // namespace LIBC_NAMESPACE_DECL
