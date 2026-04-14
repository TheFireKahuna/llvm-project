//===-- NT Job Object API declarations ----------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Umbrella header — includes both types and API declarations.
// Prefer including nt_job_types.h or nt_job_api.h directly when only
// one category is needed, to reduce include-graph weight.
//===----------------------------------------------------------------------===//
//
// Job objects are the Windows analog of POSIX process groups and resource
// limits. A job object contains one or more processes and enforces resource
// constraints (CPU time, memory, process count) and policy (kill-on-close,
// breakaway, scheduling class).
//
// POSIX mapping:
//   setpgid/getpgid    → NtCreateJobObject + NtAssignProcessToJobObject
//   setrlimit/getrlimit → NtSetInformationJobObject/NtQueryInformationJobObject
//                          with JobObjectExtendedLimitInformation
//   killpg              → NtTerminateJobObject
//   waitpid(-pgid)      → NtIsProcessInJob + completion port association
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_H

#include "src/__support/OSUtil/windows/nt/nt_job_types.h"
#include "src/__support/OSUtil/windows/nt/nt_job_api.h"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_H
