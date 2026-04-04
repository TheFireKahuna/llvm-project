//===-- NT IoRing API declarations --------------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// IoRing Syscalls
//===----------------------------------------------------------------------===//

// NtCreateIoRing — create an I/O ring and map shared SQ/CQ memory.
// CreateParameters: pointer to NT_IORING_STRUCTV1 (version, SQ/CQ sizes).
// OutputParameters: pointer to NT_IORING_INFO (receives mapped ring pointers).
// On success, the SQ and CQ are mapped into the calling process's address
// space and ready for direct userspace access.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateIoRing(HANDLE *IoRingHandle, ULONG CreateParametersLength,
               PVOID CreateParameters, ULONG OutputParametersLength,
               PVOID OutputParameters);

// NtSubmitIoRing — submit pending SQEs and optionally wait for completions.
// Flags: reserved (0).
// WaitOperations: number of CQEs to wait for before returning (0 = non-blocking).
// Timeout: maximum wait time (nullptr = infinite when WaitOperations > 0).
// This is the only syscall in the hot path — SQE push and CQE pop are pure
// userspace shared memory operations.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSubmitIoRing(HANDLE IoRingHandle, ULONG Flags, ULONG WaitOperations,
               LARGE_INTEGER *Timeout);

// NtQueryIoRingCapabilities — discover system-wide IoRing capabilities.
// Returns max version, max opcode, feature flags, and max ring sizes.
// Call once at startup to determine which IORING_OP_* codes are available.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryIoRingCapabilities(SIZE_T IoRingCapabilitiesLength,
                          PVOID IoRingCapabilities);

// NtSetInformationIoRing — configure ring parameters.
// IoRingInformationClassCompletionEvent (class 1): register a completion event
// handle. The kernel signals this event when new CQEs are posted, enabling
// integration with NtAssociateWaitCompletionPacket for IOCP-based event loops.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetInformationIoRing(HANDLE IoRingHandle, ULONG IoRingInformationClass,
                       ULONG IoRingInformationLength,
                       PVOID IoRingInformation);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_API_H
