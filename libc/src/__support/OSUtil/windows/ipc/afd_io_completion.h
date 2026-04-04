//===-- Socket AFD I/O completion via reactor IOCP ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Zero-event I/O completion for AFD sockets. Every blocking or async AFD
// ioctl routes its IRP completion through the reactor's process-wide IOCP.
// The calling thread parks on its own ThreadLocalWord (notify_word) and
// the reactor drain thread wakes it via ThreadLocalWord::alert_if_parked.
//
// No NT Event objects are created or held for I/O completion — the per-
// socket HANDLE is bound to the reactor IOCP once at socket() creation,
// and each IRP carries its `IoWaiter *` as `ApcContext`. The completion
// router recognises socket I/O keys by pointer identity against the
// singleton `socket_io_sentinel()` value and dispatches directly, without
// touching any per-socket state.
//
// Concurrency properties:
//   - Each in-flight IRP owns its own `IoWaiter`, so concurrent send+recv
//     on the same socket wait on disjoint completion state. This is the
//     fix for the shared-event hazard where a per-socket NT Event conflated
//     unrelated IRP completions.
//   - The caller's `IoWaiter` may live on its stack provided the caller
//     does not return until `done == 1`. Both `ioctl_blocking` and
//     `ioctl_nonblock` enforce this: nonblock issues cancel-then-wait so
//     the IRP completion is observed before return.
//   - The `IoWaiter` for nonblocking connect lives inside `SocketState`,
//     so it outlives the originating `connect()` call. Completion may
//     fire with `tlw == nullptr`; the router skips the alert and the
//     probe thread polls `done` non-blockingly.
//
// Reentrancy: ThreadLocalWord::wait_for_addr is not reentrant. Do not
// call ioctl_blocking / ioctl_nonblock from a VEH filter or any code
// that may run nested inside an existing wait_for_addr on the same thread.
// Normal POSIX signal handlers may call socket I/O — APC delivery unparks
// the outer wait, which resumes after the handler completes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_IO_COMPLETION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_IO_COMPLETION_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

struct ThreadLocalWord;

namespace internal {
namespace afd_io {

/// Per-IRP completion record. Lives on the caller's stack for
/// ioctl_blocking / ioctl_nonblock, or inside SocketState for persistent
/// async operations (e.g. in-flight nonblocking connect).
///
/// Field semantics:
///   - tlw         — park word to alert on completion. May be null if the
///                   caller will poll `done` instead.
///   - done        — 0 while the IRP is in flight, 1 once the router has
///                   published the completion. RELEASE-stored by the router
///                   after status/information are written.
///   - status      — final NTSTATUS once `done == 1`.
///   - information — final Information field once `done == 1`.
struct IoWaiter {
  ThreadLocalWord *tlw;
  cpp::Atomic<uint32_t> done;
  NTSTATUS status;
  ULONG_PTR information;
};

/// Singleton completion key stamped on every socket-io IRP by bind_handle().
/// The completion router dispatches by pointer identity against this value.
PVOID socket_io_sentinel();

/// Bind an AFD socket HANDLE to the reactor IOCP with Key = sentinel.
/// Every IRP submitted on this handle with a non-null ApcContext will
/// subsequently post a completion routed to the socket-io dispatcher.
/// Called once per handle at socket()/accept() time.
NTSTATUS bind_handle(HANDLE socket);

/// Blocking ioctl: issue NtDeviceIoControlFile, park the current thread
/// on its notify_word until the IRP completes, then return the final
/// NTSTATUS. `iosb` receives the kernel's Status/Information. Safe for
/// concurrent use from multiple threads on the same socket — each call
/// owns its own stack IoWaiter.
NTSTATUS ioctl_blocking(HANDLE socket, ULONG ioctl_code, void *in_buf,
                        ULONG in_len, void *out_buf, ULONG out_len,
                        IO_STATUS_BLOCK *iosb);

/// Non-blocking variant: if the kernel would block the IRP, cancel it
/// and return STATUS_DEVICE_NOT_READY. The cancel is resolved
/// synchronously (the IRP's router-published completion is observed
/// before return) so `iosb` and internal waiter state are safe to reuse.
NTSTATUS ioctl_nonblock(HANDLE socket, ULONG ioctl_code, void *in_buf,
                        ULONG in_len, void *out_buf, ULONG out_len,
                        IO_STATUS_BLOCK *iosb);

/// Submit an async IRP with a caller-owned `IoWaiter`. The waiter must
/// live until `waiter->done == 1` is observed by whoever reaps it. Used
/// by nonblocking connect: the waiter is embedded in SocketState and
/// outlives the `connect()` call.
///
/// On entry `waiter->tlw` may be null (pure polling) or point to a
/// ThreadLocalWord whose owner will later park on `&waiter->done`.
///
/// Return values:
///   STATUS_SUCCESS  — completed synchronously; waiter->done == 1,
///                     waiter->status / information are valid.
///   STATUS_PENDING  — IRP pended; the router will later publish the
///                     completion on the waiter.
///   other NTSTATUS  — synchronous error; waiter->done == 1 too.
NTSTATUS ioctl_async(HANDLE socket, ULONG ioctl_code, void *in_buf,
                     ULONG in_len, void *out_buf, ULONG out_len,
                     IoWaiter *waiter, IO_STATUS_BLOCK *iosb);

/// Park the current thread on `waiter->done` until the router publishes
/// completion. `waiter->tlw` must equal the current thread's notify_word.
/// Returns the final NTSTATUS. `iosb` is updated to mirror the router's
/// published status/information.
NTSTATUS reap_waiter(IoWaiter *waiter, IO_STATUS_BLOCK *iosb);

/// Non-blocking probe: returns STATUS_PENDING if not yet complete,
/// otherwise the final NTSTATUS. Used by socket_probe_connect_completion.
/// `iosb` is updated on completion.
NTSTATUS poll_waiter(IoWaiter *waiter, IO_STATUS_BLOCK *iosb);

/// Install the socket-io completion handler. Called from
/// socket_io_startup_init during Tier B Phase 7, after reactor_startup_init.
int startup_init();

} // namespace afd_io
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IPC_AFD_IO_COMPLETION_H
