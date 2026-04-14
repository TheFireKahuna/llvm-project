//===-- Libc-internal NT exception and context APIs ----------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Libc-internal NT APIs that build on the public platform ABI types from
// <sys/ntabi.h> (via nt_types.h). This file adds APIs used only by libc:
// NtRaiseException, NtContinue/NtContinueEx, time functions, instruction
// cache flush, etc.
//
// Shared SEH/unwind ABI types (CONTEXT, EXCEPTION_RECORD, DISPATCHER_CONTEXT,
// RtlUnwindEx, RtlCaptureContext, VEH registration, etc.) are in
// <sys/ntabi.h> and available to all runtimes.
//
//===----------------------------------------------------------------------===//
// Umbrella header — includes both types and API declarations.
// Prefer including nt_context_types.h or nt_context_api.h directly when only
// one category is needed, to reduce include-graph weight.
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_H

#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/OSUtil/windows/nt/nt_context_api.h"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CONTEXT_H
