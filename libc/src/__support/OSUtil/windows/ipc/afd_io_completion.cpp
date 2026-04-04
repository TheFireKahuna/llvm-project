//===-- Socket AFD I/O completion via reactor IOCP -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/afd_io_completion.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_file_types.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/reactor/completion_router.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_local_word.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace afd_io {

// ---------------------------------------------------------------------------
// Sentinel key
// ---------------------------------------------------------------------------
//
// Every socket-io IRP is submitted on a HANDLE bound to the reactor IOCP
// with Key = &g_socket_io_sentinel_mem. The completion router recognises
// this exact pointer value and dispatches via socket_io_router below.
//
// The value stored at the sentinel address is arbitrary — only the
// pointer's identity matters. A memorable magic value makes post-mortem
// heap inspection less ambiguous if the sentinel is ever mistakenly
// dereferenced.
static uint64_t g_socket_io_sentinel_mem = 0x5AFD'10C0'0DE0'5001ULL;

PVOID socket_io_sentinel() {
  return &g_socket_io_sentinel_mem;
}

// ---------------------------------------------------------------------------
// Router dispatch
// ---------------------------------------------------------------------------
//
// Called by the completion-router multiplexer when a completion arrives
// with key == socket_io_sentinel(). Publishes the result on the waiter
// and alerts the parked thread, if any.

static void socket_io_router(PVOID /*key*/, PVOID apc_context, NTSTATUS status,
                             ULONG_PTR information) {
  auto *w = static_cast<IoWaiter *>(apc_context);
  if (!w)
    return; // Defensive: every socket IRP supplies a waiter.

  w->status = status;
  w->information = information;
  // RELEASE pairs with ACQUIRE loads by ioctl_blocking / poll_waiter /
  // reap_waiter. Must publish status/information before done.
  w->done.store(1, cpp::MemoryOrder::RELEASE);

  if (w->tlw)
    ThreadLocalWord::alert_if_parked(w->tlw);
}

// ---------------------------------------------------------------------------
// Handle binding
// ---------------------------------------------------------------------------

NTSTATUS bind_handle(HANDLE socket) {
  FILE_COMPLETION_INFORMATION info;
  info.Port = reactor::iocp_handle();
  info.Key = socket_io_sentinel();
  IO_STATUS_BLOCK iosb = {};
  return ::NtSetInformationFile(socket, &iosb, &info, sizeof(info),
                                FileCompletionInformation);
}

// ---------------------------------------------------------------------------
// Async submission
// ---------------------------------------------------------------------------

NTSTATUS ioctl_async(HANDLE socket, ULONG ioctl_code, void *in_buf,
                     ULONG in_len, void *out_buf, ULONG out_len,
                     IoWaiter *waiter, IO_STATUS_BLOCK *iosb) {
  // Initialise the waiter before submission so the router never races
  // against uninitialised fields in the synchronous-completion case.
  waiter->status = STATUS_PENDING;
  waiter->information = 0;
  waiter->done.store(0, cpp::MemoryOrder::RELEASE);

  // NtDeviceIoControlFile: event=nullptr, apc_fn=nullptr, apc_ctx=waiter.
  // The kernel posts completion to the handle's bound IOCP with
  // ApcContext = waiter.
  NTSTATUS s = ::NtDeviceIoControlFile(
      socket, /*Event=*/nullptr, /*ApcFn=*/nullptr, /*ApcCtx=*/waiter, iosb,
      ioctl_code, in_buf, in_len, out_buf, out_len);

  if (s == STATUS_PENDING)
    return STATUS_PENDING;

  // Synchronous completion path. The kernel may OR may NOT post an IOCP
  // completion for synchronously-finished IRPs depending on FILE_SKIP_
  // COMPLETION_PORT_ON_SUCCESS. We do not set that flag, so a completion
  // DOES post even for sync success — the router will fire and publish
  // on the waiter. We populate the waiter here as well, but the router's
  // later publish is idempotent (same status/information).
  //
  // For synchronous errors, no IOCP completion posts. We publish here so
  // the caller sees `done == 1` immediately.
  waiter->status = s;
  waiter->information = iosb->Information;
  waiter->done.store(1, cpp::MemoryOrder::RELEASE);
  return s;
}

// ---------------------------------------------------------------------------
// Parking / probing
// ---------------------------------------------------------------------------

NTSTATUS reap_waiter(IoWaiter *waiter, IO_STATUS_BLOCK *iosb) {
  // Outer loop: re-check `done` after every spurious wake. wait_for_addr
  // already loops internally across APC/alert wakes but a belt-and-braces
  // re-check here costs nothing and makes the termination condition
  // unambiguous for readers.
  while (waiter->done.load(cpp::MemoryOrder::ACQUIRE) == 0) {
    // tlw must be set to current thread's notify_word by the caller.
    waiter->tlw->wait_for_addr(
        reinterpret_cast<const volatile uint32_t *>(&waiter->done), 0);
  }
  iosb->Status = waiter->status;
  iosb->Information = waiter->information;
  return waiter->status;
}

NTSTATUS poll_waiter(IoWaiter *waiter, IO_STATUS_BLOCK *iosb) {
  if (waiter->done.load(cpp::MemoryOrder::ACQUIRE) == 0)
    return STATUS_PENDING;
  iosb->Status = waiter->status;
  iosb->Information = waiter->information;
  return waiter->status;
}

// ---------------------------------------------------------------------------
// Blocking / non-blocking entry points
// ---------------------------------------------------------------------------

NTSTATUS ioctl_blocking(HANDLE socket, ULONG ioctl_code, void *in_buf,
                        ULONG in_len, void *out_buf, ULONG out_len,
                        IO_STATUS_BLOCK *iosb) {
  IoWaiter w;
  auto *lc = get_current_lifecycle();
  w.tlw = lc ? &lc->notify_word : nullptr;

  NTSTATUS s = ioctl_async(socket, ioctl_code, in_buf, in_len, out_buf, out_len,
                           &w, iosb);
  if (s != STATUS_PENDING) {
    // Synchronous completion (success or error). `iosb` already holds the
    // kernel's values. The router may still fire for sync-success cases,
    // but the waiter publish is idempotent and we can return now.
    return s;
  }

  // Park until the router publishes. If tlw is null (no lifecycle —
  // extremely early bring-up), fall back to a busy spin; this should be
  // unreachable in practice since sockets only exist post-Tier B.
  if (!w.tlw) {
    while (w.done.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      __asm__ __volatile__("pause" ::: "memory");
    }
    iosb->Status = w.status;
    iosb->Information = w.information;
    return w.status;
  }

  return reap_waiter(&w, iosb);
}

NTSTATUS ioctl_nonblock(HANDLE socket, ULONG ioctl_code, void *in_buf,
                        ULONG in_len, void *out_buf, ULONG out_len,
                        IO_STATUS_BLOCK *iosb) {
  IoWaiter w;
  auto *lc = get_current_lifecycle();
  w.tlw = lc ? &lc->notify_word : nullptr;

  NTSTATUS s = ioctl_async(socket, ioctl_code, in_buf, in_len, out_buf, out_len,
                           &w, iosb);
  if (s != STATUS_PENDING)
    return s;

  // Would block. Cancel the IRP and wait for the kernel to finalise the
  // cancellation so the stack waiter is not touched after return.
  IO_STATUS_BLOCK cancel_iosb = {};
  ::NtCancelIoFileEx(socket, iosb, &cancel_iosb);

  if (w.tlw) {
    reap_waiter(&w, iosb);
  } else {
    while (w.done.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      __asm__ __volatile__("pause" ::: "memory");
    }
    iosb->Status = w.status;
    iosb->Information = w.information;
  }
  return STATUS_DEVICE_NOT_READY;
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

int startup_init() {
  completion_router::install_sentinel_handler(socket_io_sentinel(),
                                              socket_io_router);
  return 0;
}

} // namespace afd_io

// Subsystem forwarder matching libc_subsystem_init.h's naming convention.
// Called from Tier B Phase 7, after reactor_startup_init.
int socket_io_startup_init() { return afd_io::startup_init(); }

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
