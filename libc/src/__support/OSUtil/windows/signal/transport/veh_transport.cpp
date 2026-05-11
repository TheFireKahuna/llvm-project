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
#include "src/__support/OSUtil/windows/debug/crash_handler.h"
#include "src/__support/OSUtil/windows/debug/hw_breakpoint.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/pending/pending_storage.h"
#include "src/__support/OSUtil/windows/signal/dispatch/dispatch_fwd.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"
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

// Underlying NTSTATUS values that EXCEPTION_IN_PAGE_ERROR carries in
// ExceptionInformation[2] when the demand-paged read failed because of
// media/device failure rather than a missing/unmappable address. These
// map to POSIX BUS_OBJERR ("object-specific hardware error"); everything
// else (including unmapped section regions and revoked file handles)
// stays BUS_ADRERR.
bool is_device_error_status(uintptr_t status) {
  switch (static_cast<uint32_t>(status)) {
  case 0xC000003Fu: // STATUS_CRC_ERROR
  case 0xC0000032u: // STATUS_DISK_CORRUPT_ERROR
  case 0xC000009Cu: // STATUS_DEVICE_DATA_ERROR
  case 0xC00000A3u: // STATUS_DEVICE_NOT_READY
  case 0xC00000B5u: // STATUS_IO_TIMEOUT
  case 0xC00000E9u: // STATUS_UNEXPECTED_IO_ERROR
  case 0xC0000102u: // STATUS_FILE_CORRUPT_ERROR
  case 0xC0000185u: // STATUS_IO_DEVICE_ERROR
  case 0xC0000206u: // STATUS_INSUFFICIENT_RESOURCES (paging path)
    return true;
  default:
    return false;
  }
}

// Build si_code for a given signal from the exception record + context.
//
// The CONTEXT is consulted only for SIGTRAP, where DR6 distinguishes a
// hardware-breakpoint/watchpoint hit (B0..B3) from an EFlags.TF single
// step (BS) on x86. AArch64 has no equivalent in the public CONTEXT, so
// EXCEPTION_SINGLE_STEP collapses to TRAP_TRACE there.
int build_si_code(int signum, const EXCEPTION_RECORD *rec,
                  const CONTEXT *ctx) {
  switch (signum) {
  case SIGFPE:
    return fpe_exception_to_si_code(rec->ExceptionCode);
  case SIGSEGV:
    return (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) ? SEGV_MAPERR
                                                            : SEGV_ACCERR;
  case SIGBUS:
    // EXCEPTION_DATATYPE_MISALIGNMENT — unaligned access on an arch that
    // requires alignment (or AC=1 on x86). POSIX BUS_ADRALN.
    if (rec->ExceptionCode == EXCEPTION_DATATYPE_MISALIGNMENT)
      return BUS_ADRALN;
    // EXCEPTION_IN_PAGE_ERROR carries the underlying NTSTATUS in
    // ExceptionInformation[2]. Device/media failures surface as
    // BUS_OBJERR; everything else stays BUS_ADRERR.
    if (rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR &&
        rec->NumberParameters >= 3 &&
        is_device_error_status(rec->ExceptionInformation[2])) {
      return BUS_OBJERR;
    }
    return BUS_ADRERR;
  case SIGILL:
    // EXCEPTION_PRIV_INSTRUCTION — CPL>0 attempted a CPL=0-only opcode
    // (HLT, MOV CRn, WRMSR, etc.). POSIX ILL_PRVOPC is the matching code;
    // collapsing it into ILL_ILLOPC hides the privilege violation from
    // handlers that rely on si_code to triage faults.
    if (rec->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION)
      return ILL_PRVOPC;
    return ILL_ILLOPC;
  case SIGTRAP:
    // EXCEPTION_BREAKPOINT (INT3 / BRK #0xF000) → software breakpoint.
    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT)
      return TRAP_BRKPT;
    // EXCEPTION_SINGLE_STEP — fired by either EFlags.TF (single step)
    // or a DR0..DR3 hardware breakpoint match. DR6 disambiguates:
    //   bits 0..3 (B0..B3) — hardware breakpoint condition detected
    //   bit  14   (BS)     — single-step (EFlags.TF) trap
    // BS may be co-set with B0..B3 if a step lands on a HW breakpoint;
    // we report HW-breakpoint in that case (debugger-visible cause).
#if defined(__x86_64__) || defined(_M_X64)
    if (ctx && (ctx->Dr6 & 0xFu))
      return TRAP_HWBKPT;
    if (ctx && (ctx->Dr6 & (1u << 14)))
      return TRAP_TRACE;
#endif
    return TRAP_TRACE;
  default:
    return SI_KERNEL;
  }
}

// Compute si_addr for a hardware-sourced signal.
//
// POSIX splits the meaning of si_addr by signal class:
//   SIGSEGV / SIGBUS (memory faults) — faulting virtual address
//   SIGFPE / SIGILL / SIGTRAP        — faulting instruction PC
//
// Windows surfaces the fault VA via EXCEPTION_RECORD::ExceptionInformation
// on two codes:
//   EXCEPTION_ACCESS_VIOLATION (NumberParameters == 2)
//     [0] = access type (0=read, 1=write, 8=DEP/execute)
//     [1] = faulting VA
//   EXCEPTION_IN_PAGE_ERROR    (NumberParameters == 3)
//     [0] = access type
//     [1] = faulting VA
//     [2] = underlying NTSTATUS
//
// Everything else (STACK_OVERFLOW, ARRAY_BOUNDS_EXCEEDED, misalignment,
// all FPE classes, illegal/priv instruction, breakpoint, single-step) has
// no ExceptionInformation payload — we report ExceptionAddress, which is
// the faulting instruction PC in every case and is the correct POSIX
// si_addr for SIGFPE/SIGILL/SIGTRAP. For the SEGV/BUS edge cases that
// fall through here (stack-overflow, misalignment, bounds), reporting the
// PC instead of a VA is the best we can do without further NT support
// and still gives the handler a meaningful crash site to log.
void *build_si_addr(const EXCEPTION_RECORD *rec) {
  DWORD code = rec->ExceptionCode;
  if ((code == EXCEPTION_ACCESS_VIOLATION ||
       code == EXCEPTION_IN_PAGE_ERROR) &&
      rec->NumberParameters >= 2) {
    return reinterpret_cast<void *>(rec->ExceptionInformation[1]);
  }
  return rec->ExceptionAddress;
}

// C++ exception magic number. Thrown by MSVC/Clang C++ runtime.
// We must not intercept these — let the C++ EH machinery handle them.
inline constexpr DWORD EXCEPTION_CPP = 0xE06D7363;

// CLR exception magic number.
inline constexpr DWORD EXCEPTION_CLR = 0xE0434352;

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

  // Helper for the "we are about to surrender to the OS terminator" exits.
  // Each one of those exits is a process-fatal event that, without this hook,
  // would vanish silently — the OS unhandled-exception path runs before any
  // libc code can log. crash_backtrace_from_context walks the captured
  // EXCEPTION_POINTERS::ContextRecord (i.e., the user fault site, not this
  // VEH frame) and writes a labelled trace to PEB stderr. It uses only Tier A
  // primitives (PEB Ldr + NtWriteFile + FaultGuard-protected stack walk), so
  // it is safe even for faults during __libc_init bring-up where Tier B
  // subsystems are not yet online.
  auto pass_through_with_trace = [&]() -> LONG {
    ::LIBC_NAMESPACE::internal::crash_backtrace_from_context(
        signum, rec, ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
  };

  // Get thread signal state. If the thread has no signal state (foreign
  // thread), we can't deliver signals — pass to next handler. This is also
  // the path for very-early faults (before init_signal_state has run),
  // which are the ones most worth tracing because no other diagnostic exists.
  ThreadSignalState *state = get_thread_state_noinit();
  if (!state)
    return pass_through_with_trace();

  // Stage 3: Check disposition and thread-side mask.
  //
  // Synchronous hardware faults have no safe "defer" semantics — the
  // faulting instruction will re-execute on EXCEPTION_CONTINUE_EXECUTION
  // and re-fault in a tight loop. For any disposition that wouldn't fix
  // the fault this pass, we surrender to the OS default handler (process
  // termination). This matches Linux's force_sig_info behavior for
  // synchronous signals: ignoring or blocking SIGSEGV/SIGFPE/SIGBUS/SIGILL
  // is not meaningful and must not livelock the process.
  //
  // Snapshot masks before branching. Loading ign_mask first ensures a
  // concurrent sigaction(SIG_IGN) is seen: the worst case is "ignore
  // when handler was just installed" (re-delivered next fault), never
  // "terminate when SIG_IGN was just installed" (fatal).
  uint64_t sig_bit = 1ULL << (signum - 1);
  uint64_t ign_mask =
      g_pcb.signal_handler.ignored.load(cpp::MemoryOrder::ACQUIRE);
  uint64_t custom_mask =
      g_pcb.signal_handler.custom.load(cpp::MemoryOrder::ACQUIRE);

  // SIG_IGN on a synchronous fault: returning CONTINUE_EXECUTION re-runs
  // the faulting instruction, which re-faults, which is re-ignored — an
  // infinite loop. Force SIG_DFL semantics (OS terminates). This matches
  // glibc/Linux behavior.
  if (ign_mask & sig_bit)
    return pass_through_with_trace();

  // SIG_DFL — let OS handle (terminate).
  if (!(custom_mask & sig_bit))
    return pass_through_with_trace();

  // Blocked signal on a synchronous fault: a custom handler is installed,
  // but the signal is masked in this thread. dispatch_pending would skip
  // the handler, leave the pending bit set, and return — but we can't
  // just return CONTINUE_EXECUTION, because the instruction re-faults
  // immediately and loops. Linux semantics: blocking a synchronous fault
  // is undefined, and the kernel force-terminates via force_sig_info. We
  // do the same — let the OS default handler terminate the process.
  uint64_t blocked_bits = sigset_to_bits(state->blocked_signals);
  if (blocked_bits & sig_bit)
    return pass_through_with_trace();

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
    return pass_through_with_trace();
  }

  // Build si_code + si_addr and pend to the thread's pending set. Both
  // sidecars are written before the RELEASE CAS in pend_standard, so the
  // dispatch engine reconstructs a correct siginfo_t with the proper
  // POSIX si_code (SEGV_MAPERR, FPE_INTDIV, etc.) and si_addr (faulting
  // VA for SEGV/BUS, faulting PC for FPE/ILL/TRAP) instead of hardcoding
  // SI_USER with a null address.
  int si_code = build_si_code(signum, rec, ep->ContextRecord);
  // DR6 status bits are sticky — hardware sets them and only software ever
  // clears them. We read DR6 in build_si_code to distinguish TRAP_HWBKPT
  // from TRAP_TRACE; without clearing here the next trap would OR in with
  // the stale cause and misclassify. Scoped to SIGTRAP so ACCESS_VIOLATION
  // and friends don't pay for the write.
  if (signum == SIGTRAP)
    windows::hw_bp::clear_dr6_status(ep->ContextRecord);
  void *fault_addr = build_si_addr(rec);
  (void)signal_pending::pend_standard(state->pending, signum, si_code,
                                      fault_addr);

  // Store fault info for re-fault detection.
  state->last_fault_pc = fault_pc;
  state->last_fault_code = code;

  // EXCEPTION_BREAKPOINT PC fixup — advance past the trap instruction so
  // the dispatched ucontext_t mirrors Linux SIGTRAP-from-int3 semantics.
  //
  // Windows reports EXCEPTION_BREAKPOINT as a fault: ContextRecord->Rip is
  // the trap byte itself. Returning EXCEPTION_CONTINUE_EXECUTION re-runs
  // the trap, so without this fixup the handler is invoked, returns, and
  // the second VEH entry is caught by the re-fault detector above and
  // surrendered to the OS terminator — handler effectively runs once and
  // the process dies. Linux instead delivers SIGTRAP with the saved PC
  // already past the int3, so handler-return resumes after the trap.
  //
  // We probe the byte/word at PC under FaultGuard before advancing, to
  // distinguish a real INT3/BRK from RaiseException(EXCEPTION_BREAKPOINT)
  // — there ContextRecord->Rip is a normal post-call site, not a trap
  // instruction; advancing would corrupt control flow. The probe also
  // covers the (rare) case of a trap at the last byte of an unmapped
  // page boundary, where the next-byte read itself would fault.
  //
  // si_addr was already built from ExceptionAddress (the trap byte) by
  // build_si_addr above, so the SA_SIGINFO contract — si_addr at trap,
  // ucontext at post-trap — matches Linux exactly.
  //
  // The BeingDebugged guard in master_veh_handler returns CONTINUE_SEARCH
  // for EXCEPTION_BREAKPOINT before this filter runs, so this code only
  // executes when no debugger is attached — debuggers always see the
  // trap with PC unmodified.
  if (signum == SIGTRAP && code == EXCEPTION_BREAKPOINT) {
#if defined(__x86_64__) || defined(_M_X64)
    auto *pc = reinterpret_cast<const uint8_t *>(ep->ContextRecord->Rip);
    uint8_t op;
    if (windows::safe_load_u8(pc, &op) && op == 0xCC)
      ep->ContextRecord->Rip += 1;
#elif defined(__aarch64__) || defined(_M_ARM64)
    // BRK #imm16 encoding: bits 31..21 = 11010100001, bits 4..0 = 00000.
    // Mask = 0xFFE0001F, fixed pattern = 0xD4200000.
    auto *pc = reinterpret_cast<const uint32_t *>(ep->ContextRecord->Pc);
    uint32_t insn;
    if (windows::safe_load_u32(pc, &insn) &&
        (insn & 0xFFE0001Fu) == 0xD4200000u)
      ep->ContextRecord->Pc += 4;
#endif
  }

  // Forward the interrupted CONTEXT so SA_SIGINFO handlers receive a
  // meaningful ucontext_t. The pointer is valid for the duration of this
  // VEH callback frame — ep->ContextRecord lives on the kernel-provided
  // exception frame. For synchronous exceptions (SIGSEGV, SIGFPE, etc.),
  // dispatch_pending() runs synchronously below within this same frame,
  // so the pointer remains live. Cleared after dispatch returns.
  //
  // Without this, SA_SIGINFO handlers for hardware exceptions would
  // receive a null ucontext — only the APC transport previously set it.
  //
  // The signum binding pairs the captured CONTEXT with the signal it
  // belongs to. dispatch_pending drains all pending standard signals
  // lowest-first, so without this binding any software signal queued
  // before the fault would inherit the fault site's CONTEXT — wrong.
  state->interrupted_context = ep->ContextRecord;
  state->interrupted_record = rec;
  state->interrupted_signum = signum;

  // Clear the master VEH reentry guard before handing control to user
  // code. The guard was set by master_veh_handler to gate libc filter
  // execution against recursive VEH entry; that role is finished — the
  // upcoming dispatch_pending invokes the user-installed sigaction
  // handler, which is application code permitted to do anything,
  // including siglongjmp out of this stack frame.
  //
  // RAII cleanup (master_veh_handler's VehReentryGuard destructor) does
  // not run on a non-local exit, so leaving the flag set would
  // permanently disable VEH dispatch on this thread for any subsequent
  // fault — silently turning further SIGSEGV/SIGFPE/SIGBUS into "OS
  // crash handler" passthrough. Clear here so:
  //   - normal handler return: master_veh's RAII destructor re-clears
  //     (idempotent no-op).
  //   - handler siglongjmp:    flag is already cleared, next fault
  //                            dispatches normally.
  //   - handler returns then this function returns
  //                            CONTINUE_EXECUTION, faulting instruction
  //                            re-runs (caught by re-fault check).
  internal::teb_tls_set(g_pcb.zone0.veh_sealed().reentry_tls_index,
                        nullptr);

  // Trigger dispatch via Layer 3 and dispatch immediately.
  //
  // For synchronous exceptions, we must dispatch here rather than
  // deferring to a later boundary — the faulting instruction will
  // re-execute immediately on EXCEPTION_CONTINUE_EXECUTION, giving
  // the handler a chance to fix the fault or longjmp away. If the
  // handler returns without fixing, the re-fault detection above
  // catches the repeated fault and passes it through to the OS.
  signal_dispatch::trigger(state);
  signal_dispatch::dispatch_pending(state);

  // Clear the context pointer after dispatch. Deferred signals delivered
  // later won't carry the original exception context.
  state->interrupted_context = nullptr;
  state->interrupted_record = nullptr;
  state->interrupted_signum = 0;

  return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace veh_transport
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

// ---------------------------------------------------------------------------
// Static VEH filter record — picked up by the .libcveh sweep during Tier A.
// Pre-Tier-B firing degrades naturally: handle_exception's custom_mask check
// (against g_pcb.signal_handler.custom, zero before any sigaction) returns
// EXCEPTION_CONTINUE_SEARCH when no SEH-class handler is installed.
// ---------------------------------------------------------------------------
LIBC_REGISTER_VEH_FILTER(signal_veh_transport,
                         ::LIBC_NAMESPACE::windows::VEH_ALL_SIGNAL,
                         &::LIBC_NAMESPACE::signal_state::veh_transport::
                             handle_exception,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_SIGNAL)
