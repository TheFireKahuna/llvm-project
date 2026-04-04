//===-- Libc-internal NT exception and context APIs ---------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_context_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Instruction Cache
//===----------------------------------------------------------------------===//

// Flush instruction cache. On x86_64 this is a no-op in the kernel (I-cache
// is coherent with D-cache). On ARM64 it issues the actual cache maintenance.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtFlushInstructionCache(HANDLE ProcessHandle, PVOID BaseAddress, SIZE_T Length);

//===----------------------------------------------------------------------===//
// Time Functions
//===----------------------------------------------------------------------===//

// Performance counter and precise system time (ntdll.dll). These bypass the
// kernel32/kernelbase forwarders. Both use a usermode TSC fast path via
// KUSER_SHARED_DATA — zero syscalls on modern hardware.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOL
RtlQueryPerformanceCounter(LARGE_INTEGER *PerformanceCount);
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOL
RtlQueryPerformanceFrequency(LARGE_INTEGER *PerformanceFrequency);

// Returns wall-clock time as 100ns units since 1601-01-01 (FILETIME epoch).
// Interpolates between kernel ticks using QPC for sub-microsecond precision.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR LONGLONG RtlGetSystemTimePrecise(void);

// Sets wall-clock time. NewTime is 100ns units since 1601-01-01 (FILETIME
// epoch). OldTime receives the previous value (nullable). Requires
// SeSystemtimePrivilege; returns STATUS_PRIVILEGE_NOT_HELD without it.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetSystemTime(LARGE_INTEGER *NewTime, LARGE_INTEGER *OldTime);

// Monotonic time excluding suspend, in 100ns units. Reads InterruptTime and
// InterruptTimeBias from KUSER_SHARED_DATA with a seqlock — zero syscalls.
// Matches CLOCK_MONOTONIC semantics (Linux excludes suspend).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOL
RtlQueryUnbiasedInterruptTime(ULONGLONG *UnbiasedTime);

//===----------------------------------------------------------------------===//
// Exception Raising — NtRaiseException
//===----------------------------------------------------------------------===//

// NtRaiseException — raise an exception into the kernel exception dispatcher
// with a full CPU context. The kernel dispatches to VEH/SEH handlers using
// the supplied ContextRecord (not the actual current context).
//
// ExceptionRecord: describes the exception (code, address, flags, parameters).
// ContextRecord: CPU state at the "point of exception" — handlers see this
//                context and may modify it before continuing.
// FirstChance: TRUE = give debuggers and handlers first shot at handling;
//              FALSE = skip first-chance dispatch, go directly to unhandled
//              exception processing (second chance → termination).
//
// Used for signal self-delivery via raise(): construct an EXCEPTION_RECORD
// with the signal number encoded in the exception code, build a CONTEXT
// pointing at the call site, and dispatch through the VEH signal handler.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtRaiseException(EXCEPTION_RECORD *ExceptionRecord, CONTEXT *ContextRecord,
                 BOOLEAN FirstChance);

//===----------------------------------------------------------------------===//
// Context Continuation — NtContinue / NtContinueEx
//===----------------------------------------------------------------------===//

// NtContinue — resume execution from a modified context record.
// This is the sigreturn equivalent: after a signal handler modifies the
// saved context (or leaves it unchanged), NtContinue restores the CPU
// state and resumes execution at ContextRecord->Rip/Pc.
// Alertable=TRUE tests for pending alerts before resuming (delivering
// queued APCs).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtContinue(CONTEXT *ContextRecord, BOOLEAN Alertable);

// NtContinueEx — extended continuation with type control (Win10+).
// KCONTINUE_LONGJUMP is directly useful for longjmp from signal handlers:
// the kernel knows to skip C++ destructor unwinding, matching POSIX
// longjmp semantics where signal handler longjmp bypasses cleanup.
// ContinueArgument: pointer to KCONTINUE_ARGUMENT, or nullptr for
// default behavior (equivalent to NtContinue with Alertable=FALSE).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtContinueEx(CONTEXT *ContextRecord, KCONTINUE_ARGUMENT *ContinueArgument);

//===----------------------------------------------------------------------===//
// Stack Back Trace Capture — RtlCaptureStackBackTrace
//===----------------------------------------------------------------------===//

// Captures a stack back trace by walking the call stack. Returns the number
// of frames captured. FramesToSkip excludes that many frames from the top
// (caller). FramesToCapture is the maximum number of frames to record.
// BackTrace receives the return addresses. BackTraceHash is optional.
//
// This is the fast path for backtrace() — single NT call, no DbgHelp,
// no symbol engine. Uses RtlVirtualUnwind internally on x64/ARM64.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR USHORT
RtlCaptureStackBackTrace(ULONG FramesToSkip, ULONG FramesToCapture,
                         PVOID *BackTrace, PULONG BackTraceHash);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_API_H
