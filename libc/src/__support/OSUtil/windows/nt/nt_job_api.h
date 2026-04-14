//===-- NT Job Object API declarations ----------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_API_H

#include "src/__support/OSUtil/windows/nt/nt_job_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Job Object Syscalls
//===----------------------------------------------------------------------===//

// NtCreateJobObject — create a job object.
// Named jobs (via ObjectAttributes) are cross-process. Unnamed jobs are
// private to the creating process. The job starts with no processes and
// no limits — assign processes with NtAssignProcessToJobObject and set
// limits with NtSetInformationJobObject.
__declspec(dllimport) NTSTATUS NTAPI
NtCreateJobObject(HANDLE *JobHandle, ACCESS_MASK DesiredAccess,
                  PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtOpenJobObject — open an existing named job object.
__declspec(dllimport) NTSTATUS NTAPI
NtOpenJobObject(HANDLE *JobHandle, ACCESS_MASK DesiredAccess,
                PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtAssignProcessToJobObject — add a process to a job.
// JobHandle requires JOB_OBJECT_ASSIGN_PROCESS access.
// ProcessHandle requires PROCESS_SET_QUOTA | PROCESS_TERMINATE access.
// A process can only be assigned to one job (unless nested job support is
// enabled). Once assigned, the process cannot be removed from the job.
__declspec(dllimport) NTSTATUS NTAPI
NtAssignProcessToJobObject(HANDLE JobHandle, HANDLE ProcessHandle);

// NtTerminateJobObject — terminate all processes in the job.
// If the job is nested, all processes in child jobs are also terminated.
// Implements killpg() — send SIGKILL to an entire process group.
// JobHandle requires JOB_OBJECT_TERMINATE access.
__declspec(dllimport) NTSTATUS NTAPI
NtTerminateJobObject(HANDLE JobHandle, NTSTATUS ExitStatus);

__declspec(dllimport) NTSTATUS NTAPI
NtIsProcessInJob(HANDLE ProcessHandle, HANDLE JobHandle);

// NtQueryInformationJobObject — query job object attributes.
// JobHandle may be nullptr to query the job associated with the calling
// process. JobObjectInformationClass selects the structure format.
// ReturnLength receives the required buffer size on STATUS_BUFFER_TOO_SMALL.
__declspec(dllimport) NTSTATUS NTAPI
NtQueryInformationJobObject(HANDLE JobHandle,
                            ULONG JobObjectInformationClass,
                            PVOID JobObjectInformation,
                            ULONG JobObjectInformationLength,
                            ULONG *ReturnLength);

// NtSetInformationJobObject — set job object attributes.
// Primary use: JobObjectExtendedLimitInformation (class 9) for setrlimit().
__declspec(dllimport) NTSTATUS NTAPI
NtSetInformationJobObject(HANDLE JobHandle, ULONG JobObjectInformationClass,
                          PVOID JobObjectInformation,
                          ULONG JobObjectInformationLength);

// NtCreateJobSet — create a nested job hierarchy from an array of job handles.
// Each entry specifies a MemberLevel that determines nesting depth.
// Used for multi-level process group hierarchies.
__declspec(dllimport) NTSTATUS NTAPI
NtCreateJobSet(ULONG NumJob, JOB_SET_ARRAY *UserJobSet, ULONG Flags);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_API_H
