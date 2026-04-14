//===-- NT Job Object constants and structures ---------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Job Object Access Rights
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK JOB_OBJECT_ASSIGN_PROCESS = 0x0001;
inline constexpr ACCESS_MASK JOB_OBJECT_SET_ATTRIBUTES = 0x0002;
inline constexpr ACCESS_MASK JOB_OBJECT_QUERY = 0x0004;
inline constexpr ACCESS_MASK JOB_OBJECT_TERMINATE = 0x0008;
inline constexpr ACCESS_MASK JOB_OBJECT_SET_SECURITY_ATTRIBUTES = 0x0010;
// STANDARD_RIGHTS_REQUIRED (0x000F0000) | SYNCHRONIZE (0x00100000) | 0x3F
inline constexpr ACCESS_MASK JOB_OBJECT_ALL_ACCESS = 0x001F003F;

//===----------------------------------------------------------------------===//
// I/O Counters
//===----------------------------------------------------------------------===//

// Per-process or per-job I/O transfer statistics.
// Used in JOBOBJECT_EXTENDED_LIMIT_INFORMATION and
// JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION.
struct IO_COUNTERS {
  ULONGLONG ReadOperationCount;
  ULONGLONG WriteOperationCount;
  ULONGLONG OtherOperationCount;
  ULONGLONG ReadTransferCount;
  ULONGLONG WriteTransferCount;
  ULONGLONG OtherTransferCount;
};

//===----------------------------------------------------------------------===//
// Job Object Information Classes
//===----------------------------------------------------------------------===//

// JOBOBJECTINFOCLASS constants for NtQueryInformationJobObject /
// NtSetInformationJobObject. Only the classes relevant to POSIX resource
// limits and process group management are declared here.
inline constexpr ULONG JobObjectBasicAccountingInformation = 1;
inline constexpr ULONG JobObjectBasicLimitInformation = 2;
inline constexpr ULONG JobObjectBasicProcessIdList = 3;
inline constexpr ULONG JobObjectBasicUIRestrictions = 4;
inline constexpr ULONG JobObjectEndOfJobTimeInformation = 6;
inline constexpr ULONG JobObjectAssociateCompletionPortInformation = 7;
inline constexpr ULONG JobObjectBasicAndIoAccountingInformation = 8;
inline constexpr ULONG JobObjectExtendedLimitInformation = 9;
inline constexpr ULONG JobObjectGroupInformation = 11;
inline constexpr ULONG JobObjectNotificationLimitInformation = 12;
inline constexpr ULONG JobObjectCpuRateControlInformation = 15;
inline constexpr ULONG JobObjectNetRateControlInformation = 32;

//===----------------------------------------------------------------------===//
// Limit Flags — JOBOBJECT_BASIC_LIMIT_INFORMATION.LimitFlags
//===----------------------------------------------------------------------===//

// Working set size limits (MinimumWorkingSetSize / MaximumWorkingSetSize).
inline constexpr DWORD JOB_OBJECT_LIMIT_WORKINGSET = 0x00000001;
// Per-process user-mode CPU time limit (PerProcessUserTimeLimit).
inline constexpr DWORD JOB_OBJECT_LIMIT_PROCESS_TIME = 0x00000002;
// Per-job user-mode CPU time limit (PerJobUserTimeLimit).
inline constexpr DWORD JOB_OBJECT_LIMIT_JOB_TIME = 0x00000004;
// Maximum number of simultaneously active processes (ActiveProcessLimit).
inline constexpr DWORD JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 0x00000008;
// Processor affinity for all processes (Affinity).
inline constexpr DWORD JOB_OBJECT_LIMIT_AFFINITY = 0x00000010;
// Priority class for all processes (PriorityClass).
inline constexpr DWORD JOB_OBJECT_LIMIT_PRIORITY_CLASS = 0x00000020;
// Preserve previously accumulated per-job time when setting new limits.
inline constexpr DWORD JOB_OBJECT_LIMIT_PRESERVE_JOB_TIME = 0x00000040;
// Scheduling class (0–9) for all processes (SchedulingClass).
inline constexpr DWORD JOB_OBJECT_LIMIT_SCHEDULING_CLASS = 0x00000080;
// Per-process committed memory limit (ProcessMemoryLimit in extended info).
inline constexpr DWORD JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100;
// Per-job committed memory limit (JobMemoryLimit in extended info).
inline constexpr DWORD JOB_OBJECT_LIMIT_JOB_MEMORY = 0x00000200;
// Terminate unhandled exceptions instead of showing the error dialog.
inline constexpr DWORD JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION = 0x00000400;
// Allow child processes to break away from the job (CREATE_BREAKAWAY_FROM_JOB).
inline constexpr DWORD JOB_OBJECT_LIMIT_BREAKAWAY_OK = 0x00000800;
// Child processes automatically break away from the job.
inline constexpr DWORD JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK = 0x00001000;
// Terminate all processes when the last job handle is closed.
inline constexpr DWORD JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
// Limit affinity to a subset of the job's affinity mask.
inline constexpr DWORD JOB_OBJECT_LIMIT_SUBSET_AFFINITY = 0x00004000;
// Low memory notification threshold (JobMemoryLow in extended info v2).
inline constexpr DWORD JOB_OBJECT_LIMIT_JOB_MEMORY_LOW = 0x00008000;
// I/O byte limits.
inline constexpr DWORD JOB_OBJECT_LIMIT_JOB_READ_BYTES = 0x00010000;
inline constexpr DWORD JOB_OBJECT_LIMIT_JOB_WRITE_BYTES = 0x00020000;
// CPU rate control (use with JobObjectCpuRateControlInformation).
inline constexpr DWORD JOB_OBJECT_LIMIT_CPU_RATE_CONTROL = 0x00040000;
inline constexpr DWORD JOB_OBJECT_LIMIT_IO_RATE_CONTROL = 0x00080000;
inline constexpr DWORD JOB_OBJECT_LIMIT_NET_RATE_CONTROL = 0x00100000;

//===----------------------------------------------------------------------===//
// Job Object Information Structures
//===----------------------------------------------------------------------===//

// Accounting information — cumulative CPU time and process counts.
// Returned by NtQueryInformationJobObject(JobObjectBasicAccountingInformation).
struct JOBOBJECT_BASIC_ACCOUNTING_INFORMATION {
  LARGE_INTEGER TotalUserTime;             // Cumulative user-mode CPU time
  LARGE_INTEGER TotalKernelTime;           // Cumulative kernel-mode CPU time
  LARGE_INTEGER ThisPeriodTotalUserTime;   // User time since last limit reset
  LARGE_INTEGER ThisPeriodTotalKernelTime; // Kernel time since last limit reset
  DWORD TotalPageFaultCount;               // Cumulative page faults
  DWORD TotalProcesses;                    // Total processes ever assigned
  DWORD ActiveProcesses;                   // Currently active processes
  DWORD TotalTerminatedProcesses;          // Processes terminated due to limits
};

// Basic limit information — resource constraints.
// Used with JobObjectBasicLimitInformation (class 2).
// LimitFlags indicates which fields are active.
struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
  LARGE_INTEGER PerProcessUserTimeLimit; // Max user-mode CPU per process (100ns)
  LARGE_INTEGER PerJobUserTimeLimit;     // Max user-mode CPU for entire job (100ns)
  DWORD LimitFlags;                      // JOB_OBJECT_LIMIT_* bitmask
  SIZE_T MinimumWorkingSetSize;          // Minimum working set (bytes)
  SIZE_T MaximumWorkingSetSize;          // Maximum working set (bytes)
  DWORD ActiveProcessLimit;              // Max simultaneously active processes
  ULONG_PTR Affinity;                    // Processor affinity mask
  DWORD PriorityClass;                   // PROCESS_PRIORITY_CLASS_* value
  DWORD SchedulingClass;                 // Scheduling class (0–9)
};

// Extended limit information — adds memory limits and I/O accounting.
// Used with JobObjectExtendedLimitInformation (class 9).
// This is the primary structure for setrlimit/getrlimit implementation.
struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
  JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
  IO_COUNTERS IoInfo;           // I/O transfer statistics
  SIZE_T ProcessMemoryLimit;    // Per-process commit limit (bytes)
  SIZE_T JobMemoryLimit;        // Per-job commit limit (bytes)
  SIZE_T PeakProcessMemoryUsed; // Peak per-process commit usage
  SIZE_T PeakJobMemoryUsed;     // Peak job-wide commit usage
};

// Process ID list — enumerate processes in a job.
// Returned by NtQueryInformationJobObject(JobObjectBasicProcessIdList).
// Variable-length: ProcessIdList extends past the struct.
struct JOBOBJECT_BASIC_PROCESS_ID_LIST {
  DWORD NumberOfAssignedProcesses; // Total processes in the job
  DWORD NumberOfProcessIdsInList;  // Entries returned in ProcessIdList
  ULONG_PTR ProcessIdList[1];     // Variable-length PID array
};

// Combined accounting + I/O information.
// Returned by NtQueryInformationJobObject(JobObjectBasicAndIoAccountingInformation).
struct JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION {
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION BasicInfo;
  IO_COUNTERS IoInfo;
};

// Associate a job with an I/O completion port for notifications.
// Set with NtSetInformationJobObject(JobObjectAssociateCompletionPortInformation).
// Notifications are posted for events like process exit, limit violation, etc.
struct JOBOBJECT_ASSOCIATE_COMPLETION_PORT {
  PVOID CompletionKey;    // Opaque key returned in completion packets
  HANDLE CompletionPort;  // I/O completion port handle
};

// CPU rate control — limit CPU usage as a percentage or weight.
// Used with JobObjectCpuRateControlInformation (class 15).
struct JOBOBJECT_CPU_RATE_CONTROL_INFORMATION {
  ULONG ControlFlags;
  union {
    ULONG CpuRate;          // CPU rate as percentage * 100 (e.g. 5000 = 50%)
    ULONG Weight;           // Scheduling weight (1–9)
    struct {
      USHORT MinRate;       // Minimum CPU rate percentage * 100
      USHORT MaxRate;       // Maximum CPU rate percentage * 100
    };
  };
};

// CPU rate control flags.
inline constexpr ULONG JOB_OBJECT_CPU_RATE_CONTROL_ENABLE = 0x1;
inline constexpr ULONG JOB_OBJECT_CPU_RATE_CONTROL_WEIGHT_BASED = 0x2;
inline constexpr ULONG JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP = 0x4;
inline constexpr ULONG JOB_OBJECT_CPU_RATE_CONTROL_NOTIFY = 0x8;
inline constexpr ULONG JOB_OBJECT_CPU_RATE_CONTROL_MIN_MAX_RATE = 0x10;

// Job set entry — used with NtCreateJobSet to create nested job hierarchies.
struct JOB_SET_ARRAY {
  HANDLE JobHandle;   // Job object handle
  DWORD MemberLevel;  // Nesting level (must be > 0, can be sparse)
  DWORD Flags;        // Reserved, must be 0
};

// NtIsProcessInJob — test whether a process belongs to a job.
// Returns STATUS_PROCESS_IN_JOB if the process is in the specified job,
// STATUS_PROCESS_NOT_IN_JOB if not. If JobHandle is nullptr, tests whether
// the process is in any job at all.
inline constexpr NTSTATUS STATUS_PROCESS_IN_JOB =
    static_cast<NTSTATUS>(0x00000121);
inline constexpr NTSTATUS STATUS_PROCESS_NOT_IN_JOB =
    static_cast<NTSTATUS>(0x00000122);

//===----------------------------------------------------------------------===//
// End-of-Job Time Action
//===----------------------------------------------------------------------===//

// Controls what happens when JOB_OBJECT_LIMIT_JOB_TIME fires.
// Set via NtSetInformationJobObject(JobObjectEndOfJobTimeInformation).
inline constexpr DWORD JOB_OBJECT_TERMINATE_AT_END_OF_JOB = 0;
inline constexpr DWORD JOB_OBJECT_POST_AT_END_OF_JOB = 1;

struct JOBOBJECT_END_OF_JOB_TIME_INFORMATION {
  DWORD EndOfJobTimeAction;
};

//===----------------------------------------------------------------------===//
// Job Object Completion Port Message IDs
//===----------------------------------------------------------------------===//

// Posted to the IOCP associated via JobObjectAssociateCompletionPortInformation.
// The message ID appears in IoStatusBlock.Information (dwNumberOfBytesTransferred
// in Win32 terms). The process ID appears in ApcContext (lpOverlapped in Win32).
inline constexpr ULONG JOB_OBJECT_MSG_END_OF_JOB_TIME = 1;
inline constexpr ULONG JOB_OBJECT_MSG_END_OF_PROCESS_TIME = 2;
inline constexpr ULONG JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT = 3;
inline constexpr ULONG JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO = 4;
inline constexpr ULONG JOB_OBJECT_MSG_NEW_PROCESS = 6;
inline constexpr ULONG JOB_OBJECT_MSG_EXIT_PROCESS = 7;
inline constexpr ULONG JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS = 8;
inline constexpr ULONG JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT = 9;
inline constexpr ULONG JOB_OBJECT_MSG_JOB_MEMORY_LIMIT = 10;
inline constexpr ULONG JOB_OBJECT_MSG_NOTIFICATION_LIMIT = 11;

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_JOB_TYPES_H
