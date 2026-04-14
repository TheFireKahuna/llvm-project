//===-- NT Worker Factory API declarations ------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_API_H

#include "src/__support/OSUtil/windows/nt/nt_threadpool_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Worker Factory Syscalls
//===----------------------------------------------------------------------===//

// NtCreateWorkerFactory — create a kernel thread pool bound to a completion
// port. The factory auto-creates worker threads (up to MaxThreadCount) that
// call StartRoutine(StartParameter) and then enter the wait/dispatch loop.
//
// CompletionPortHandle: I/O completion port that feeds work to this factory.
//   Work is queued by posting completions to this port (NtSetIoCompletionEx).
// WorkerProcessHandle: process in which threads are created (NtCurrentProcess()
//   for same-process).
// StartRoutine: thread entry point. Each worker calls this on creation, then
//   enters the NtWorkerFactoryWorkerReady → NtWaitForWorkViaWorkerFactory loop.
// StartParameter: opaque parameter passed to StartRoutine.
// MaxThreadCount: maximum number of worker threads (0 = use system default
//   based on processor count).
// StackReserve/StackCommit: per-thread stack sizes (0 = defaults from PE header).
__declspec(dllimport) NTSTATUS NTAPI
NtCreateWorkerFactory(HANDLE *WorkerFactoryHandleReturn,
                      ACCESS_MASK DesiredAccess,
                      PCOBJECT_ATTRIBUTES ObjectAttributes,
                      HANDLE CompletionPortHandle, HANDLE WorkerProcessHandle,
                      PVOID StartRoutine, PVOID StartParameter,
                      ULONG MaxThreadCount, SIZE_T StackReserve,
                      SIZE_T StackCommit);

// NtQueryInformationWorkerFactory — query factory configuration and thread
// pool state. WorkerFactoryBasicInformation returns the full snapshot.
__declspec(dllimport) NTSTATUS NTAPI
NtQueryInformationWorkerFactory(
    HANDLE WorkerFactoryHandle,
    WORKERFACTORYINFOCLASS WorkerFactoryInformationClass,
    PVOID WorkerFactoryInformation, ULONG WorkerFactoryInformationLength,
    ULONG *ReturnLength);

// NtSetInformationWorkerFactory — configure factory parameters.
// Common uses:
//   WorkerFactoryThreadMinimum: set minimum persistent thread count.
//   WorkerFactoryThreadMaximum: set maximum thread count.
//   WorkerFactoryIdleTimeout: how long idle threads wait before exiting.
//   WorkerFactoryPaused: pause/resume thread creation.
__declspec(dllimport) NTSTATUS NTAPI
NtSetInformationWorkerFactory(HANDLE WorkerFactoryHandle,
                              WORKERFACTORYINFOCLASS WorkerFactoryInformationClass,
                              PVOID WorkerFactoryInformation,
                              ULONG WorkerFactoryInformationLength);

// NtWorkerFactoryWorkerReady — signal that a worker thread has initialized
// and is ready to receive work. Each worker thread must call this once after
// StartRoutine completes its per-thread initialization, before entering the
// NtWaitForWorkViaWorkerFactory loop. The kernel uses this to track the
// ready worker count and decide whether to create additional threads.
__declspec(dllimport) NTSTATUS NTAPI
NtWorkerFactoryWorkerReady(HANDLE WorkerFactoryHandle);

// NtWaitForWorkViaWorkerFactory — block until work is available, then
// dequeue completion packets. This is the worker thread's main dispatch call.
// Returns one or more FILE_IO_COMPLETION_INFORMATION entries (same format
// as NtRemoveIoCompletionEx). PacketsReturned receives the actual count.
// DeferredWork: optional WORKER_FACTORY_DEFERRED_WORK for ALPC integration,
// or nullptr for standard completion port dispatch.
__declspec(dllimport) NTSTATUS NTAPI
NtWaitForWorkViaWorkerFactory(
    HANDLE WorkerFactoryHandle,
    FILE_IO_COMPLETION_INFORMATION *MiniPackets, ULONG Count,
    ULONG *PacketsReturned, PVOID DeferredWork);

// NtReleaseWorkerFactoryWorker — release a worker thread from the factory.
// Decrements the factory's thread count, allowing the calling thread to exit
// the worker loop gracefully. The kernel may create a replacement thread if
// the count falls below the minimum.
__declspec(dllimport) NTSTATUS NTAPI
NtReleaseWorkerFactoryWorker(HANDLE WorkerFactoryHandle);

// NtShutdownWorkerFactory — initiate orderly shutdown of the factory.
// PendingWorkerCount is in/out: set it to the number of workers you expect
// to still be active. The kernel signals all waiting workers to exit.
// After this call, no new threads will be created and
// NtWaitForWorkViaWorkerFactory returns STATUS_SHUTDOWN_IN_PROGRESS for
// all workers.
__declspec(dllimport) NTSTATUS NTAPI
NtShutdownWorkerFactory(HANDLE WorkerFactoryHandle,
                        volatile LONG *PendingWorkerCount);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_API_H
