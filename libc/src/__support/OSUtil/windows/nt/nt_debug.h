//===-- NT debugger output (DbgPrintEx) ----------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// `DbgPrintEx` is an ntdll RTL export, not a syscall. The call chain is:
//
//   DbgPrintEx (ntdll)
//     -> internal vsnprintf into a stack buffer
//     -> vDbgPrintExWithPrefix
//     -> RtlRaiseException(DBG_PRINTEXCEPTION_C = 0x40010006)
//     -> NtRaiseException
//
// An attached debugger intercepts the 0x40010006 exception code and prints
// the buffer; with no debugger attached the exception is swallowed and the
// call is effectively a no-op from the process's perspective.
//
// `DbgPrint` is equivalent to `DbgPrintEx(DPFLTR_DEFAULT_ID,
// DPFLTR_INFO_LEVEL, ...)` — we prefer the Ex form so output is filterable
// by component/level in WinDbg (`Kd_*_Mask`, `!dbgprint`).
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_DEBUG_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_DEBUG_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

// DPFLTR component IDs and severity levels (ntddk.h). DPFLTR_DEFAULT_ID is
// the generic bucket used by user-mode diagnostics; DPFLTR_INFO_LEVEL is
// the lowest severity that still prints under a default `Kd_DEFAULT_Mask`.
#define DPFLTR_DEFAULT_ID      0x65
#define DPFLTR_ERROR_LEVEL     0
#define DPFLTR_WARNING_LEVEL   1
#define DPFLTR_TRACE_LEVEL     2
#define DPFLTR_INFO_LEVEL      3

extern "C" {

// Sends a formatted message to the attached kernel/user debugger.
//   ComponentId — DPFLTR_*_ID bucket (use DPFLTR_DEFAULT_ID for libc).
//   Level       — DPFLTR_*_LEVEL severity.
//   Format, ... — printf-style format string and arguments.
// Returns the number of characters written.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR unsigned long
DbgPrintEx(unsigned long ComponentId, unsigned long Level,
           const char *Format, ...);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_DEBUG_H
