//===-- NT Worker Factory (thread pool) API declarations ----- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Umbrella header — includes both types and API declarations.
// Prefer including nt_threadpool_types.h or nt_threadpool_api.h directly when
// only one category is needed, to reduce include-graph weight.
//===----------------------------------------------------------------------===//
//
// Worker factories are the kernel-level primitive behind the NT thread pool.
// A worker factory manages a pool of threads that dequeue work items from an
// associated I/O completion port. The kernel automatically creates and
// destroys threads based on demand, within configured min/max bounds.
//
// This is the mechanism underlying TpAllocPool/TpSubmitWork and the Win32
// thread pool APIs. Direct access enables:
//   - Custom thread pool for aio_* (POSIX async I/O)
//   - Low-overhead work dispatch without kernel32/ntdll pool overhead
//   - Fine-grained control over thread count, stack sizes, and timeouts
//
// Flow:
//   1. Create I/O completion port (NtCreateIoCompletion)
//   2. Create worker factory bound to that port (NtCreateWorkerFactory)
//   3. Worker threads call NtWorkerFactoryWorkerReady then
//      NtWaitForWorkViaWorkerFactory in a loop
//   4. Queue work by posting to the completion port (NtSetIoCompletionEx)
//   5. NtShutdownWorkerFactory to drain and stop all workers
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_H

#include "src/__support/OSUtil/windows/nt/nt_threadpool_types.h"
#include "src/__support/OSUtil/windows/nt/nt_threadpool_api.h"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_THREADPOOL_H
