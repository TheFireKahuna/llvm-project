//===-- Console transport (Layer 2c) --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ctrl-C/Break/Close/Shutdown/Logoff → POSIX signal delivery.
//
// Preserved from signal_handlers.cpp with the following changes:
//   - Calls signal_pending::pend_standard() (Layer 1) on the process-wide
//     pending set instead of deliver_process_signal().
//   - Calls signal_dispatch::trigger_any_thread() (Layer 3) to wake a
//     suitable thread for dispatch.
//
// The console handler runs on a dedicated OS thread, not the thread that
// should handle the signal. The trigger_any_thread() call picks the best
// candidate (preferred_thread fast path, then registry walk).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/transport/console_transport.h"

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_fwd.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace {

using console::CTRL_BREAK_EVENT;
using console::CTRL_C_EVENT;
using console::CTRL_CLOSE_EVENT;
using console::CTRL_LOGOFF_EVENT;
using console::CTRL_SHUTDOWN_EVENT;

// Map console control event to POSIX signal number.
int ctrl_event_to_signal(DWORD event) {
  switch (event) {
  case CTRL_C_EVENT:
    return SIGINT;
  case CTRL_BREAK_EVENT:
    return SIGQUIT;
  case CTRL_CLOSE_EVENT:
    return SIGHUP;
  case CTRL_SHUTDOWN_EVENT:
    return SIGTERM;
  case CTRL_LOGOFF_EVENT:
    return SIGHUP;
  default:
    return 0;
  }
}

// CAS guard for idempotent install/remove.
// Raw storage avoids global constructor from cpp::Atomic.
alignas(cpp::Atomic<bool>) static unsigned char
    handler_installed_storage[sizeof(cpp::Atomic<bool>)] = {};

cpp::Atomic<bool> &handler_installed =
    *reinterpret_cast<cpp::Atomic<bool> *>(handler_installed_storage);

} // namespace

namespace console_transport {

WINAPI DWORD handle_ctrl_event(ULONG_PTR ctrl_type) {
  // Guard: if remove() was called, stop handling events. Windows doesn't
  // support clean unregistration of ctrl handlers, so this flag check is
  // the mechanism to quiesce the handler after removal.
  if (!handler_installed.load(cpp::MemoryOrder::ACQUIRE))
    return FALSE;

  int signum = ctrl_event_to_signal(static_cast<DWORD>(ctrl_type));
  if (signum == 0)
    return FALSE; // Unknown event — pass to OS.

  // Fast disposition check without locking.
  uint64_t sig_bit = 1ULL << (signum - 1);

  // SIG_IGN: suppress the event entirely.
  uint64_t ign_mask =
      g_pcb.signal_handler.ignored.load(cpp::MemoryOrder::ACQUIRE);
  if (ign_mask & sig_bit)
    return TRUE; // Handled — suppress OS default.

  // SIG_DFL: let the OS default handler run (terminates the process for
  // CTRL_C, CTRL_CLOSE, etc.).
  uint64_t custom_mask =
      g_pcb.signal_handler.custom.load(cpp::MemoryOrder::ACQUIRE);
  if (!(custom_mask & sig_bit))
    return FALSE; // SIG_DFL — pass to OS.

  // Custom handler installed. Pend to process-wide pending set.
  // Console events are process-directed — any unblocking thread handles them.
  // SI_KERNEL: console Ctrl events originate from the kernel console subsystem.
  (void)signal_pending::pend_standard(g_pcb.signal_dispatch.process_pending,
                                      signum, SI_KERNEL);

  // Wake a suitable thread for dispatch.
  signal_dispatch::trigger_any_thread();

  return TRUE; // Handled — suppress OS default.
}

void install() {
  bool expected = false;
  if (!handler_installed.compare_exchange_strong(expected, true,
                                                 cpp::MemoryOrder::ACQ_REL))
    return; // Already installed.

  // Register via CFG-safe console control registration.
  // cfg_register_call_target() is called inside console::register_ctrl()
  // to make the callback valid for Control Flow Guard.
  console::register_ctrl(handle_ctrl_event);
}

void remove() {
  bool expected = true;
  if (!handler_installed.compare_exchange_strong(expected, false,
                                                 cpp::MemoryOrder::ACQ_REL))
    return; // Not installed.

  // Note: Windows doesn't provide a clean unregister for the ctrl dispatch
  // function. We rely on the process exiting or the handler returning FALSE
  // for events after removal. The installed flag prevents re-delivery.
}

} // namespace console_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
