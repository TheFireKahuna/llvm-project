//===-- Console transport (Layer 2c) ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Console control handler transport for Ctrl-C/Break/Close/Shutdown/Logoff.
// Maps Windows console events to POSIX signal numbers and pends via Layer 1.
//
// Event → signal mapping:
//   CTRL_C_EVENT     → SIGINT
//   CTRL_BREAK_EVENT → SIGQUIT
//   CTRL_CLOSE_EVENT → SIGHUP
//   CTRL_SHUTDOWN_EVENT → SIGTERM
//   CTRL_LOGOFF_EVENT → SIGHUP
//
// Disposition fast-path: checks g_pcb.signal_handler.custom
// bitmask before pending. SIG_IGN → suppress (return TRUE), SIG_DFL →
// pass through (return FALSE for OS default termination).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_CONSOLE_TRANSPORT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_CONSOLE_TRANSPORT_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace console_transport {

// Console ctrl dispatch callback. Registered via console::register_ctrl().
// Runs on a dedicated thread created remotely in this process by the host's
// winsrv/UserCreateCallbackThread path. The single thread parameter is the
// raw console event code (CTRL_C / BREAK / CLOSE / LOGOFF / SHUTDOWN).
DWORD WINAPI handle_ctrl_event(ULONG_PTR ctrl_type);

// Install/remove the console handler. Idempotent, CAS-guarded.
void install();
void remove();

} // namespace console_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_TRANSPORT_CONSOLE_TRANSPORT_H
