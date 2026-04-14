//===-- VEH transport (Layer 2a) ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hardware exception → POSIX signal delivery. Three-stage filter:
//
//   Stage 1: Severity gate — skip informational/status (noncontinuable check).
//   Stage 2: Language runtime skip — C++ (0xE06D7363), CLR (0xE0434352).
//   Stage 3: Allowlist — only recognized exception codes produce signals.
//
// Preserved from signal_handlers.cpp with the following changes:
//   - Calls signal_pending::pend_standard() (Layer 1) instead of pend_signal().
//   - Calls signal_dispatch::trigger() (Layer 3) instead of direct CAS.
//   - Returns EXCEPTION_CONTINUE_EXECUTION after pend — dispatch runs at
//     the next dispatch boundary, not synchronously from VEH context.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/transport/veh_transport.h"

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_fwd.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace {

// ---------------------------------------------------------------------------
// Exception code → signal number mapping
// ---------------------------------------------------------------------------

// Map Windows exception code to POSIX signal number.
// Returns 0 for unrecognized/filtered exceptions.
int exception_to_signal(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return SIGSEGV;
  case EXCEPTION_IN_PAGE_ERROR:
    return SIGBUS;
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    return SIGBUS;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    return SIGSEGV;
  case EXCEPTION_STACK_OVERFLOW:
    return SIGSEGV;

  // Arithmetic exceptions → SIGFPE
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_INT_OVERFLOW:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
  case EXCEPTION_FLT_OVERFLOW:
  case EXCEPTION_FLT_UNDERFLOW:
  case EXCEPTION_FLT_INEXACT_RESULT:
  case EXCEPTION_FLT_INVALID_OPERATION:
  case EXCEPTION_FLT_DENORMAL_OPERAND:
  case EXCEPTION_FLT_STACK_CHECK:
    return SIGFPE;

  // Illegal/privileged instruction → SIGILL
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
    return SIGILL;

  // Breakpoint / single step → SIGTRAP
  case EXCEPTION_BREAKPOINT:
  case EXCEPTION_SINGLE_STEP:
    return SIGTRAP;

  default:
    return 0; // Unrecognized — pass to next handler
  }
}

// Map FPE exception code to si_code.
int fpe_exception_to_si_code(DWORD code) {
  switch (code) {
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return FPE_INTDIV;
  case EXCEPTION_INT_OVERFLOW:
    return FPE_INTOVF;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    return FPE_FLTDIV;
  case EXCEPTION_FLT_OVERFLOW:
    return FPE_FLTOVF;
  case EXCEPTION_FLT_UNDERFLOW:
    return FPE_FLTUND;
  case EXCEPTION_FLT_INEXACT_RESULT:
    return FPE_FLTRES;
  case EXCEPTION_FLT_INVALID_OPERATION:
    return FPE_FLTINV;
  case EXCEPTION_FLT_DENORMAL_OPERAND:
    return FPE_FLTINV; // No exact POSIX mapping; use FLTINV.
  case EXCEPTION_FLT_STACK_CHECK:
    return FPE_FLTINV; // Same.
  default:
    return FPE_FLTINV;
  }
}

// Build si_code for a given signal from the exception record.
int build_si_code(int signum, const EXCEPTION_RECORD *rec) {
  switch (signum) {
  case SIGFPE:
    return fpe_exception_to_si_code(rec->ExceptionCode);
  case SIGSEGV:
    return (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) ? SEGV_MAPERR
                                                            : SEGV_ACCERR;
  case SIGBUS:
    return BUS_ADRERR;
  case SIGILL:
    return ILL_ILLOPC;
  case SIGTRAP:
    return TRAP_BRKPT;
  default:
    return SI_KERNEL;
  }
}

// C++ exception magic number. Thrown by MSVC/Clang C++ runtime.
// We must not intercept these — let the C++ EH machinery handle them.
inline constexpr DWORD EXCEPTION_CPP = 0xE06D7363;

// CLR exception magic number.
inline constexpr DWORD EXCEPTION_CLR = 0xE0434352;

// Guard page violation status code.
inline constexpr DWORD STATUS_GUARD_PAGE_VIOLATION = 0x80000001;

} // namespace

// ---------------------------------------------------------------------------
// VEH filter implementation
// ---------------------------------------------------------------------------

namespace veh_transport {

LONG handle_exception(EXCEPTION_POINTERS *ep) {
  EXCEPTION_RECORD *rec = ep->ExceptionRecord;
  DWORD code = rec->ExceptionCode;

  // Stage 1: Skip language runtime exceptions immediately.
  if (code == EXCEPTION_CPP || code == EXCEPTION_CLR)
    return EXCEPTION_CONTINUE_SEARCH;

  // Stage 1b: Skip guard page violations — those are stack growth probes
  // handled by the OS or our memory fault handler.
  if (code == STATUS_GUARD_PAGE_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;

  // Stage 2: Map exception code to signal number.
  int signum = exception_to_signal(code);
  if (signum == 0)
    return EXCEPTION_CONTINUE_SEARCH;

  // SIGBUS from demand-read I/O error. The memory filter bumps the
  // generation counter on I/O error; consume() matches exactly once per
  // set(), preventing stale flags from prior faults from promoting
  // unrelated ACCESS_VIOLATIONs.
  if (signum == SIGSEGV && windows::g_pending_sigbus.consume()) {
    signum = SIGBUS;
  }

  // Get thread signal state. If the thread has no signal state (foreign
  // thread), we can't deliver signals — pass to next handler.
  ThreadSignalState *state = get_thread_state_noinit();
  if (!state)
    return EXCEPTION_CONTINUE_SEARCH;

  // Stage 3: Check if any custom handler is installed for this signal.
  // If the disposition is SIG_DFL or SIG_IGN and this is a synchronous
  // exception, we should let the OS default handler run (terminate).
  // Only intercept when a custom handler is installed.
  //
  // Snapshot both masks before branching. Loading ign_mask first ensures
  // that a concurrent sigaction(SIG_IGN) is seen: the worst case is
  // "ignore when handler was just installed" (re-delivered next fault),
  // never "terminate when SIG_IGN was just installed" (fatal).
  uint64_t sig_bit = 1ULL << (signum - 1);
  uint64_t ign_mask =
      g_pcb.signal_handler.ignored.load(cpp::MemoryOrder::ACQUIRE);
  uint64_t custom_mask =
      g_pcb.signal_handler.custom.load(cpp::MemoryOrder::ACQUIRE);
  if (ign_mask & sig_bit)
    return EXCEPTION_CONTINUE_EXECUTION; // Ignored — suppress.
  if (!(custom_mask & sig_bit))
    return EXCEPTION_CONTINUE_SEARCH;    // SIG_DFL — let OS handle.

  // Re-fault detection: if the same PC + code faults again before the
  // dispatch engine has run, the handler hasn't had a chance to fix it
  // (synchronous re-execution of the faulting instruction). Pass through
  // to let the OS default handler terminate the process.
  //
  // The dispatch engine clears last_fault_pc at entry, so unrelated faults
  // at the same PC after handler invocation are correctly treated as new.
  void *fault_pc = rec->ExceptionAddress;
  if (fault_pc == state->last_fault_pc &&
      code == state->last_fault_code) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Build si_code and store exception context for SA_SIGINFO delivery.
  int si_code = build_si_code(signum, rec);

  // Pend to the thread's pending set.
  signal_pending::pend_standard(state->pending, signum);

  // Store fault info for re-fault detection and SA_SIGINFO delivery.
  // The dispatch engine reads these when invoking the handler.
  state->last_fault_pc = fault_pc;
  state->last_fault_code = code;
  state->last_fault_si_code = si_code;

  // Trigger dispatch via Layer 3. The dispatch engine will drain pending
  // signals at the next dispatch boundary.
  //
  // For synchronous exceptions (SIGSEGV, SIGFPE, etc.), we return
  // EXCEPTION_CONTINUE_EXECUTION. The CPU will re-execute the faulting
  // instruction, but the dispatch engine runs first (at the head of the
  // VEH chain or via injected APC) and invokes the handler. If the
  // handler longjmps or fixes the fault, execution continues correctly.
  // If the handler returns, the re-fault detection above catches the
  // repeated fault and passes it through to the OS.
  signal_dispatch::trigger(state);

  return EXCEPTION_CONTINUE_EXECUTION;
}

void install() {
  windows::VehFilter filter;
  filter.exception_mask = windows::VEH_ALL_SIGNAL;
  filter.handler = handle_exception;
  filter.priority = windows::VEH_PRIORITY_SIGNAL;
  windows::register_veh_filter(filter);
}

void remove() { windows::unregister_veh_filter(handle_exception); }

} // namespace veh_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
