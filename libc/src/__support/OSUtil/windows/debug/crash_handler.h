//===-- Crash backtrace handler ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Async-signal-safe crash backtrace for default signal termination.
//
// Properties:
//   - Zero heap allocation (stack-only formatting).
//   - Writes directly to PEB stderr handle via NtWriteFile.
//   - No fd_table lookup, no FILE* buffering, no locks.
//   - Safe from VEH, APC, and signal handler contexts.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_CRASH_HANDLER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_CRASH_HANDLER_H

#include "src/__support/OSUtil/windows/nt/nt_types.h" // EXCEPTION_RECORD, CONTEXT
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Print a crash backtrace to stderr.
///
/// Called by execute_default_action() for core-dump signals (SIGSEGV, SIGFPE,
/// SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGQUIT). Captures the current thread's
/// register state, walks the stack, formats each frame, and writes directly
/// to the PEB stderr handle.
///
/// \p signum is the signal that triggered the crash (used in the header line).
///
/// Does NOT terminate the process — the caller is responsible for calling
/// NtTerminateProcess after this function returns.
void crash_backtrace(int signum);

/// Print a crash backtrace from a captured exception CONTEXT.
///
/// Called from VEH passthrough paths where a synchronous hardware fault is
/// about to be surrendered to the OS unhandled-exception terminator (no
/// handler installed, signal blocked, or re-fault). The header line includes
/// the exception code, faulting PC, and faulting VA so a fault that kills
/// the process before main() runs leaves a usable trace on stderr.
///
/// Walks from \p ctx (the EXCEPTION_POINTERS::ContextRecord) rather than the
/// VEH frame, so the printed stack is the user-mode call site of the fault,
/// not "VEH → master_veh_handler → handle_exception".
///
/// Async-signal-safe: uses only PEB Ldr, NtWriteFile, and stack-only
/// formatting buffers. Bounded stack budget ~3KB.
void crash_backtrace_from_context(int signum, const EXCEPTION_RECORD *rec,
                                  const CONTEXT *ctx);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_CRASH_HANDLER_H
