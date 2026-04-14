//===-- NT Worker Factory constants and structures ------------ *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

// FILE_IO_COMPLETION_INFORMATION is defined in nt_file.h.
struct FILE_IO_COMPLETION_INFORMATION;

extern "C" {

//===----------------------------------------------------------------------===//
// Worker Factory Access Rights
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK WORKER_FACTORY_RELEASE_WORKER = 0x0001;
inline constexpr ACCESS_MASK WORKER_FACTORY_WAIT = 0x0002;
inline constexpr ACCESS_MASK WORKER_FACTORY_SET_INFORMATION = 0x0004;
inline constexpr ACCESS_MASK WORKER_FACTORY_QUERY_INFORMATION = 0x0008;
inline constexpr ACCESS_MASK WORKER_FACTORY_READY_WORKER = 0x0010;
inline constexpr ACCESS_MASK WORKER_FACTORY_SHUTDOWN = 0x0020;
// STANDARD_RIGHTS_REQUIRED (0x000F0000) | all specific rights
inline constexpr ACCESS_MASK WORKER_FACTORY_ALL_ACCESS = 0x000F003F;

//===----------------------------------------------------------------------===//
// Worker Factory Information Classes
//===----------------------------------------------------------------------===//

enum WORKERFACTORYINFOCLASS : ULONG {
  WorkerFactoryTimeout = 0,           // qs: LARGE_INTEGER
  WorkerFactoryRetryTimeout = 1,      // qs: LARGE_INTEGER
  WorkerFactoryIdleTimeout = 2,       // s: LARGE_INTEGER
  WorkerFactoryBindingCount = 3,      // s: ULONG
  WorkerFactoryThreadMinimum = 4,     // s: ULONG
  WorkerFactoryThreadMaximum = 5,     // s: ULONG
  WorkerFactoryPaused = 6,            // qs: ULONG (boolean)
  WorkerFactoryBasicInformation = 7,  // q: WORKER_FACTORY_BASIC_INFORMATION
  WorkerFactoryAdjustThreadGoal = 8,
  WorkerFactoryCallbackType = 9,
  WorkerFactoryStackInformation = 10,
  WorkerFactoryThreadBasePriority = 11, // s: ULONG
  WorkerFactoryTimeoutWaiters = 12,     // s: ULONG (Threshold+)
  WorkerFactoryFlags = 13,              // s: ULONG
  WorkerFactoryThreadSoftMaximum = 14,  // s: ULONG
  WorkerFactoryThreadCpuSets = 15,      // (RS5+)
  MaxWorkerFactoryInfoClass
};

//===----------------------------------------------------------------------===//
// Worker Factory Structures
//===----------------------------------------------------------------------===//

// Returned by NtQueryInformationWorkerFactory(WorkerFactoryBasicInformation).
// Provides a complete snapshot of the worker factory's configuration and
// current thread pool state.
struct WORKER_FACTORY_BASIC_INFORMATION {
  LARGE_INTEGER Timeout;         // Work item timeout
  LARGE_INTEGER RetryTimeout;    // Retry timeout for failed thread creation
  LARGE_INTEGER IdleTimeout;     // Idle thread timeout before destruction
  BOOLEAN Paused;                // TRUE if factory is paused (no new threads)
  BOOLEAN TimerSet;              // TRUE if timeout timer is active
  BOOLEAN QueuedToExWorker;      // TRUE if queued to executive worker thread
  BOOLEAN MayCreate;             // TRUE if factory is allowed to create threads
  BOOLEAN CreateInProgress;      // TRUE if thread creation is in progress
  BOOLEAN InsertedIntoQueue;     // TRUE if work item is in the queue
  BOOLEAN Shutdown;              // TRUE if factory is shutting down
  ULONG BindingCount;            // Number of bindings
  ULONG ThreadMinimum;           // Minimum persistent thread count
  ULONG ThreadMaximum;           // Maximum thread count
  ULONG PendingWorkerCount;      // Threads with work pending
  ULONG WaitingWorkerCount;      // Threads idle, waiting for work
  ULONG TotalWorkerCount;        // Total threads currently alive
  ULONG ReleaseCount;            // Cumulative release count
  LONGLONG InfiniteWaitGoal;     // Threads that should wait indefinitely
  PVOID StartRoutine;            // Thread entry point
  PVOID StartParameter;          // Opaque parameter passed to StartRoutine
  HANDLE ProcessId;              // Owner process
  SIZE_T StackReserve;           // Stack reservation size per thread
  SIZE_T StackCommit;            // Stack commit size per thread
  NTSTATUS LastThreadCreationStatus; // Status of last thread creation attempt
};

// Deferred work descriptor — used with NtWaitForWorkViaWorkerFactory to
// optionally perform ALPC message sends as part of the wait cycle.
struct WORKER_FACTORY_DEFERRED_WORK {
  PVOID AlpcSendMessage;      // PPORT_MESSAGE to send
  PVOID AlpcSendMessagePort;  // ALPC port handle
  ULONG AlpcSendMessageFlags; // Send flags
  ULONG Flags;                // Reserved
};

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_TYPES_H
