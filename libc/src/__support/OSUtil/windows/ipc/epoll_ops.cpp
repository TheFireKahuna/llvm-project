//===-- Reactor-backed epoll engine implementation -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine functions for epoll_create1, epoll_ctl, and epoll_wait.
// All return long: non-negative value on success, -errno on failure.
//
// Unified IOCP model:
//   All event sources (AFD sockets, WCP bridges, synthetic posts) target
//   the reactor's process-wide IOCP via reactor::iocp_handle(). The
//   reactor's drain thread routes non-reactor completions to per-instance
//   pending queues. epoll_wait blocks on a per-instance ready_event.
//
// Before blocking, epoll_wait does a non-blocking IOCP flush to pick up
// freshly-arrived completions. During the flush, reactor-keyed completions
// are dispatched inline via reactor::dispatch_inline(), and epoll-keyed
// completions are routed to the appropriate instance's pending queue.
//
// Registration dispatch:
//   AFD sockets:     IOCTL_AFD_NOTIFY -> reactor::iocp_handle().
//   Socketpair/FIFO: WCP bridges -> reactor::iocp_handle().
//   Regular files:   Synthetic immediate completion via NtSetIoCompletionEx.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/epoll_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/struct_epoll_event.h"
#include "include/llvm-libc-macros/sys-epoll-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_epoll.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// Anonymous namespace helpers
//===----------------------------------------------------------------------===//

namespace {

// Router installation guard -- ensures we install the completion router
// exactly once (on first epoll_create1 call).
cpp::Atomic<bool> g_router_installed{false};

// Global router activity counter. Tracks how many threads are currently
// inside epoll_completion_router. The close path waits for this to reach
// zero after tombstoning all registrations, ensuring no in-flight router
// invocation holds a pointer into the dying instance's regs[] or inst.
//
// Atomic increment at router entry, decrement at exit. The close path's
// tombstone (reg->instance = nullptr, RELEASE) ensures any router that
// starts AFTER the tombstone will see nullptr and bail immediately.
// Waiting for g_router_active == 0 ensures all PRE-tombstone invocations
// (which may hold non-null inst pointers) have completed.
cpp::Atomic<uint32_t> g_router_active{0};

//===----------------------------------------------------------------------===//
// Completion router -- installed with the reactor
//===----------------------------------------------------------------------===//
//
// Called by the reactor's drain thread for every non-reactor completion,
// and inline by epoll_wait's flush_shared_iocp() on arbitrary threads.
//
// Safety contract:
//   The key is an EpollRegistration* pointing into an EpollInstance's
//   regs[] page. On close(epfd), the close path tombstones reg->instance
//   to nullptr (RELEASE) before freeing any pages. This router loads
//   reg->instance (ACQUIRE) — if nullptr, the registration is dead and
//   all fields may be stale, so bail immediately.
//
//   The global g_router_active counter brackets the entire router body.
//   The close path waits for g_router_active == 0 after tombstoning,
//   guaranteeing no thread holds a pointer into the freed pages.

void epoll_completion_router(PVOID key, PVOID /*apc_context*/,
                             NTSTATUS status, ULONG_PTR information) {
  // Announce entry — the close path will spin on this reaching zero.
  g_router_active.fetch_add(1, cpp::MemoryOrder::RELAXED);

  auto router_exit = [&] {
    if (g_router_active.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1)
      futex_addr::wake(&g_router_active, 1);
  };

  auto *reg = reinterpret_cast<internal::EpollRegistration *>(key);
  if (!reg) {
    router_exit();
    return;
  }

  // Load the instance pointer with ACQUIRE ordering. This pairs with
  // the RELEASE store in epoll_instance_shutdown() that sets it to
  // nullptr. If we see nullptr, the instance is being destroyed —
  // bail without touching any other fields (they may be stale/freed).
  internal::EpollInstance *inst =
      reg->instance.load(cpp::MemoryOrder::ACQUIRE);
  if (!inst || reg->fd < 0) {
    router_exit();
    return;
  }

  // Validate generation: if the fd was closed and the slot reused since
  // this completion was queued, the generation won't match. Drop stale
  // completions silently rather than routing them to the wrong fd.
  auto *slot = internal::fd_table.get_slot(reg->fd);
  if (!slot ||
      slot->load_tagged(cpp::MemoryOrder::RELAXED).gen() != reg->generation) {
    router_exit();
    return;
  }

  // Park on drain_seq when queue is full — epoll_wait increments drain_seq
  // after consuming entries and wakes us. Bounded retries as a safety net.
  {
    uint32_t seq = inst->drain_seq.load(cpp::MemoryOrder::ACQUIRE);
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (internal::epoll_push_pending(inst, key, status, information))
        goto pushed;
      // Signal the consumer to drain.
      ::NtSetEvent(inst->ready_event, nullptr);
      LARGE_INTEGER park_timeout;
      park_timeout.QuadPart = -500000; // 50ms
      futex_addr::wait_nt(&inst->drain_seq, seq, &park_timeout);
      seq = inst->drain_seq.load(cpp::MemoryOrder::ACQUIRE);
    }
  }
  // After retries the queue is still full. Drop the event — the
  // registration remains armed and will fire again on the next I/O
  // completion, so this is a missed wakeup, not a permanent loss.
  router_exit();
  return;
pushed:
  ::NtSetEvent(inst->ready_event, nullptr);
  router_exit();
}

// Ensure the completion router is installed. Called lazily from
// epoll_create1. Thread-safe via atomic flag.
void ensure_router_installed() {
  if (g_router_installed.load(cpp::MemoryOrder::ACQUIRE))
    return;
  // Benign race: multiple threads may call set_completion_router with
  // the same function pointer. The last one wins, and they're all the same.
  internal::reactor::set_completion_router(epoll_completion_router);
  g_router_installed.store(true, cpp::MemoryOrder::RELEASE);
}

//===----------------------------------------------------------------------===//
// Non-blocking IOCP flush -- used by epoll_wait
//===----------------------------------------------------------------------===//
//
// Dequeues all available completions from the shared IOCP without blocking.
// Reactor-keyed completions are dispatched inline. Epoll-keyed completions
// are routed to their owning instance's pending queue.

void flush_shared_iocp() {
  static constexpr ULONG FLUSH_BATCH = 32;
  FILE_IO_COMPLETION_INFORMATION entries[FLUSH_BATCH];

  LARGE_INTEGER zero_timeout;
  zero_timeout.QuadPart = 0;

  // Bracket the entire flush with g_router_active. This covers the gap
  // between NtRemoveIoCompletionEx returning completion keys and the
  // router being called — without this, the close path could observe
  // g_router_active == 0 while this thread holds raw EpollRegistration*
  // pointers on its stack from a dequeued batch.
  g_router_active.fetch_add(1, cpp::MemoryOrder::RELAXED);

  // Drain all available completions — not just one batch. If more than
  // FLUSH_BATCH are pending, a single dequeue leaves events queued and a
  // subsequent epoll_wait could block despite ready events.
  for (;;) {
    ULONG count = 0;
    NTSTATUS st = ::NtRemoveIoCompletionEx(
        internal::reactor::iocp_handle(), entries, FLUSH_BATCH, &count,
        &zero_timeout, FALSE); // Non-blocking, non-alertable.

    if (!NT_SUCCESS(st) || count == 0)
      break;

    for (ULONG i = 0; i < count; ++i) {
      PVOID key = entries[i].KeyContext;
      if (!key)
        continue; // Wakeup sentinel.

      if (internal::reactor::is_reactor_key(key)) {
        // Reactor completion -- dispatch inline (e.g., signal delivery).
        internal::reactor::dispatch_inline(key,
                                           entries[i].IoStatusBlock.Status,
                                           entries[i].IoStatusBlock.Information);
      } else {
        // Epoll completion -- route to instance pending queue.
        // NOTE: the router also increments/decrements g_router_active
        // internally, but that's harmless — the outer bracket here is
        // what matters for the dequeue-to-router gap coverage.
        epoll_completion_router(key, entries[i].ApcContext,
                                entries[i].IoStatusBlock.Status,
                                entries[i].IoStatusBlock.Information);
      }
    }

    // If we got fewer than a full batch, the IOCP is drained.
    if (count < FLUSH_BATCH)
      break;
  }

  if (g_router_active.fetch_sub(1, cpp::MemoryOrder::ACQ_REL) == 1)
    futex_addr::wake(&g_router_active, 1);
}

//===----------------------------------------------------------------------===//
// Translate epoll events -> AFD_NOTIFY event filter
//===----------------------------------------------------------------------===//

USHORT epoll_to_afd_filter(uint32_t events) {
  USHORT filter = SOCK_NOTIFY_REGISTER_EVENT_HANGUP;
  if (events & (EPOLLIN | EPOLLRDNORM))
    filter |= SOCK_NOTIFY_REGISTER_EVENT_IN;
  if (events & (EPOLLOUT | EPOLLWRNORM))
    filter |= SOCK_NOTIFY_REGISTER_EVENT_OUT;
  return filter;
}

UCHAR epoll_to_afd_trigger(uint32_t events) {
  UCHAR trigger = 0;
  if (events & EPOLLET)
    trigger |= SOCK_NOTIFY_TRIGGER_EDGE;
  else
    trigger |= SOCK_NOTIFY_TRIGGER_LEVEL;
  if (events & EPOLLONESHOT)
    trigger |= SOCK_NOTIFY_TRIGGER_ONESHOT;
  else
    trigger |= SOCK_NOTIFY_TRIGGER_PERSISTENT;
  return trigger;
}

//===----------------------------------------------------------------------===//
// Register AFD socket via IOCTL_AFD_NOTIFY
//===----------------------------------------------------------------------===//

int register_afd(HANDLE socket_handle, internal::EpollRegistration *reg) {
  SOCK_NOTIFY_REGISTRATION snr = {};
  snr.Socket = socket_handle;
  snr.CompletionKey = reinterpret_cast<PVOID>(reg);
  snr.EventFilter = epoll_to_afd_filter(reg->events);
  snr.Operation = SOCK_NOTIFY_OP_ENABLE;
  snr.TriggerFlags = epoll_to_afd_trigger(reg->events);

  AFD_NOTIFY_INPUT ni = {};
  ni.CompletionPort = internal::reactor::iocp_handle();
  ni.Registrations = &snr;
  ni.RegistrationCount = 1;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = NtDeviceIoControlFile(socket_handle, nullptr, nullptr, nullptr,
                                     &iosb, IOCTL_AFD_NOTIFY, &ni, sizeof(ni),
                                     nullptr, 0);
  if (!NT_SUCCESS(s))
    return -1;
  return (snr.RegistrationResult <= 1) ? 0 : -1;
}

//===----------------------------------------------------------------------===//
// Register non-AFD fd via WaitCompletionPacket bridge
//===----------------------------------------------------------------------===//

int register_wcp(HANDLE event, internal::EpollRegistration *reg,
                 HANDLE *wcp_out) {
  HANDLE wcp = nullptr;
  auto wcp_oa = windows::internal_oa();
  NTSTATUS s = NtCreateWaitCompletionPacket(
      &wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS, &wcp_oa);
  if (!NT_SUCCESS(s))
    return -1;

  BOOLEAN already = FALSE;
  s = NtAssociateWaitCompletionPacket(wcp, internal::reactor::iocp_handle(),
                                      event, reinterpret_cast<PVOID>(reg),
                                      nullptr, STATUS_SUCCESS, 0, &already);
  if (!NT_SUCCESS(s)) {
    NtClose(wcp);
    return -1;
  }

  *wcp_out = wcp;
  return 0;
}

//===----------------------------------------------------------------------===//
// Re-arm a WCP bridge after one-shot signal delivery
//===----------------------------------------------------------------------===//

void rearm_wcp(HANDLE wcp, HANDLE event, internal::EpollRegistration *reg) {
  NtClearEvent(event);
  BOOLEAN already = FALSE;
  NtAssociateWaitCompletionPacket(wcp, internal::reactor::iocp_handle(), event,
                                  reinterpret_cast<PVOID>(reg), nullptr,
                                  STATUS_SUCCESS, 0, &already);
}

//===----------------------------------------------------------------------===//
// Pipe sentinel read — zero-byte async NtReadFile on a message-mode duplicate
//===----------------------------------------------------------------------===//

// Issue a zero-byte async read on the sentinel handle. The read pends until
// data arrives, then completes with STATUS_BUFFER_OVERFLOW (message mode)
// without consuming any data. The completion posts to the reactor IOCP with
// the EpollRegistration* as the key.
// Issue a zero-byte async read on the sentinel handle. The read pends until
// data arrives, then completes with STATUS_BUFFER_OVERFLOW (message mode)
// without consuming any data. The completion posts to the reactor IOCP
// with the EpollRegistration* as the key (set via FileCompletionInformation).
//
// NOTE: The Length=0 + FILE_PIPE_MESSAGE_MODE + FILE_PIPE_QUEUE_OPERATION
// combination must pend on an empty pipe. If a kernel version returns
// STATUS_SUCCESS/0 bytes immediately instead, this sentinel approach breaks
// and needs a 1-byte read with a lookahead buffer fallback.
void issue_sentinel_read(internal::EpollRegistration *reg) {
  reg->sentinel_iosb.Status = STATUS_PENDING;
  reg->sentinel_iosb.Information = 0;
  ::NtReadFile(reg->sentinel_handle, nullptr, nullptr, nullptr,
               &reg->sentinel_iosb, nullptr, 0, nullptr, nullptr);
}

// Create a sentinel handle for a pipe fd and issue the initial read.
// Returns 0 on success, -1 on failure.
int setup_pipe_sentinel(internal::EpollRegistration *reg, HANDLE pipe_handle) {
  HANDLE sentinel = nullptr;

  // 1. Duplicate the pipe read handle.
  NTSTATUS s = ::NtDuplicateObject(
      NtCurrentProcess(), pipe_handle, NtCurrentProcess(), &sentinel,
      FILE_GENERIC_READ, 0, 0);
  if (!NT_SUCCESS(s))
    return -1;

  // 2. Switch duplicate to message-read mode so zero-byte reads pend
  //    until a message arrives, then complete with STATUS_BUFFER_OVERFLOW
  //    without consuming data.
  FILE_PIPE_INFORMATION pipe_mode = {};
  pipe_mode.ReadMode = FILE_PIPE_MESSAGE_MODE;
  pipe_mode.CompletionMode = FILE_PIPE_QUEUE_OPERATION; // blocking
  IO_STATUS_BLOCK iosb = {};
  s = ::NtSetInformationFile(sentinel, &iosb, &pipe_mode, sizeof(pipe_mode),
                             FilePipeInformation);
  if (!NT_SUCCESS(s)) {
    NtClose(sentinel);
    return -1;
  }

  // 3. Associate sentinel handle with reactor IOCP.
  FILE_COMPLETION_INFORMATION comp_info = {};
  comp_info.Port = internal::reactor::iocp_handle();
  comp_info.Key = reinterpret_cast<PVOID>(reg);
  iosb = {};
  s = ::NtSetInformationFile(sentinel, &iosb, &comp_info, sizeof(comp_info),
                             FileCompletionInformation);
  if (!NT_SUCCESS(s)) {
    NtClose(sentinel);
    return -1;
  }

  reg->sentinel_handle = sentinel;

  // 4. Issue the initial zero-byte read.
  issue_sentinel_read(reg);
  return 0;
}

//===----------------------------------------------------------------------===//
// Translate IOCP completion -> epoll_event
//===----------------------------------------------------------------------===//

uint32_t completion_to_epoll(NTSTATUS status, ULONG_PTR information,
                             const internal::EpollRegistration *reg) {
  uint32_t ev = 0;

  if (reg->is_afd) {
    if (information & SOCK_NOTIFY_EVENT_IN)
      ev |= EPOLLIN;
    if (information & SOCK_NOTIFY_EVENT_OUT)
      ev |= EPOLLOUT;
    // SOCK_NOTIFY_EVENT_HANGUP = peer shutdown(SHUT_WR) — half-close.
    // Report EPOLLRDHUP (if registered) and EPOLLIN (EOF is readable).
    // Do NOT report EPOLLHUP — SOCK_NOTIFY_EVENT_OUT in information
    // reflects what *fired*, not socket state, so we can't reliably
    // infer full close here. EPOLLHUP comes via ERR (RST/abort).
    if (information & SOCK_NOTIFY_EVENT_HANGUP) {
      ev |= reg->events & EPOLLRDHUP;
      ev |= reg->events & (EPOLLIN | EPOLLRDNORM);
    }
    // ERR = connection reset / abort. Both directions dead.
    if (information & SOCK_NOTIFY_EVENT_ERR) {
      ev |= EPOLLERR | EPOLLHUP;
      ev |= reg->events & EPOLLRDHUP;
    }
  } else if (reg->is_pipe) {
    // Sentinel read completed: STATUS_BUFFER_OVERFLOW = data available
    // (zero-byte message-mode read), STATUS_PIPE_BROKEN = writer closed.
    if (status == STATUS_BUFFER_OVERFLOW)
      ev |= reg->events & (EPOLLIN | EPOLLRDNORM);
    else if (status == STATUS_PIPE_BROKEN ||
             status == STATUS_PIPE_CLOSING) {
      ev |= EPOLLHUP;
      ev |= reg->events & EPOLLRDHUP;
    } else if (status == STATUS_CANCELLED)
      ev = 0; // Sentinel was cancelled (DEL or close), ignore.
    else
      ev |= EPOLLERR;
  } else {
    // WCP-bridged fds (FIFO, socketpair, etc.). The WCP doesn't
    // distinguish which event fired, so pass through the requested
    // I/O events. EPOLLRDHUP requires a writer-state check — deferred
    // to drain_pending() where we have fd access.
    ev = reg->events & (EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR);
  }

  return ev;
}

//===----------------------------------------------------------------------===//
// Drain pending queue into events array, with re-arm and ET suppression.
// Called under inst->pending_lock by both Phase 2 and Phase 4.
//===----------------------------------------------------------------------===//

int drain_pending(internal::EpollInstance *inst, struct epoll_event *events,
                  int maxevents, int result) {
  while (inst->pending_head != inst->pending_tail && result < maxevents) {
    auto &entry = inst->pending[inst->pending_head];
    auto *reg = reinterpret_cast<internal::EpollRegistration *>(entry.key);

    if (reg && reg->fd >= 0) {
      uint32_t ev = completion_to_epoll(entry.status, entry.information, reg);

      // EPOLLET suppression for pipes: if we already delivered and the
      // pipe hasn't drained, check before delivering again.
      if (ev && reg->is_pipe && reg->pipe_et_suppressed) {
        auto *check_ofd = internal::fd_table.get_ofd(reg->fd);
        if (check_ofd) {
          IO_STATUS_BLOCK pi_iosb = {};
          FILE_PIPE_LOCAL_INFORMATION pi = {};
          NTSTATUS ps = ::NtQueryInformationFile(
              check_ofd->handle, &pi_iosb, &pi, sizeof(pi),
              FilePipeLocalInformation);
          if (NT_SUCCESS(ps) && pi.ReadDataAvailable == 0) {
            // Pipe drained — new edge. Reset and deliver.
            reg->pipe_et_suppressed = false;
          } else {
            // Still has data — suppress. Do NOT re-arm sentinel here
            // to avoid busy-loop. The sentinel stays dormant; re-arm
            // happens in rearm_suppressed_pipes() on next epoll_wait.
            ev = 0;
          }
        }
      }

      // EPOLLRDHUP refinement for WCP-bridged fds: check whether the
      // write side of the read channel is actually gone before reporting.
      // completion_to_epoll() omits EPOLLRDHUP for WCP paths; we add it
      // here only when writer_count == 0, avoiding false positives on
      // ordinary data-available wakeups.
      if (ev && !reg->is_afd && !reg->is_pipe &&
          (reg->events & EPOLLRDHUP)) {
        auto *rdhup_ofd = internal::fd_table.get_ofd(reg->fd);
        if (rdhup_ofd) {
          internal::FifoHeader *hdr = nullptr;
          if (rdhup_ofd->kind == internal::FileKind::Fifo) {
            auto *ch = rdhup_ofd->fifo_channel();
            if (ch)
              hdr = ch->header();
          } else if (rdhup_ofd->kind == internal::FileKind::SocketPair) {
            auto *ch = rdhup_ofd->socket_pair()->read_ch;
            if (ch)
              hdr = ch->header();
          }
          if (hdr &&
              hdr->writer_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
            ev |= EPOLLRDHUP;
            // Half-close: report EPOLLIN (EOF readable) but not EPOLLHUP
            // unless the write side is also dead.
            ev |= reg->events & (EPOLLIN | EPOLLRDNORM);
          }
        }
      }

      if (ev) {
        events[result].events = ev;
        events[result].data.u64 = reg->data;
        ++result;

        // Re-arm notification sources (one-shot by design).
        if (reg->is_pipe && reg->sentinel_handle) {
          if (reg->events & EPOLLET)
            reg->pipe_et_suppressed = true;
          issue_sentinel_read(reg);
        } else if (!reg->is_afd) {
          auto *target_ofd = internal::fd_table.get_ofd(reg->fd);
          if (!target_ofd) {
            if (reg->wcp_in)
              NtCancelWaitCompletionPacket(reg->wcp_in, TRUE);
            if (reg->wcp_out)
              NtCancelWaitCompletionPacket(reg->wcp_out, TRUE);
          } else {
            if (reg->wcp_in) {
              internal::FifoChannel *read_ch = nullptr;
              if (target_ofd->kind == internal::FileKind::Fifo)
                read_ch = target_ofd->fifo_channel();
              else if (target_ofd->kind == internal::FileKind::SocketPair)
                read_ch = target_ofd->socket_pair()->read_ch;
              if (read_ch)
                rearm_wcp(reg->wcp_in, read_ch->readable_event, reg);
            }
            if (reg->wcp_out) {
              internal::FifoChannel *write_ch = nullptr;
              if (target_ofd->kind == internal::FileKind::Fifo)
                write_ch = target_ofd->fifo_channel();
              else if (target_ofd->kind == internal::FileKind::SocketPair)
                write_ch = target_ofd->socket_pair()->write_ch;
              if (write_ch)
                rearm_wcp(reg->wcp_out, write_ch->writable_event, reg);
            }
          }
        }
      }
    }
    inst->pending_head =
        (inst->pending_head + 1) % internal::EPOLL_PENDING_CAPACITY;
  }
  return result;
}

//===----------------------------------------------------------------------===//
// Re-arm sentinel reads for ET-suppressed pipes that have since drained.
// Called at the start of epoll_wait to recover from dormant sentinels.
//===----------------------------------------------------------------------===//

void rearm_suppressed_pipes(internal::EpollInstance *inst) {
  for (uint32_t i = 0; i < inst->capacity; ++i) {
    auto &reg = inst->regs[i];
    if (reg.fd < 0 || !reg.is_pipe || !reg.pipe_et_suppressed)
      continue;
    if (!reg.sentinel_handle)
      continue;

    auto *ofd = internal::fd_table.get_ofd(reg.fd);
    if (!ofd)
      continue;

    IO_STATUS_BLOCK iosb = {};
    FILE_PIPE_LOCAL_INFORMATION pi = {};
    NTSTATUS ps = ::NtQueryInformationFile(ofd->handle, &iosb, &pi,
                                           sizeof(pi), FilePipeLocalInformation);
    if (NT_SUCCESS(ps) && pi.ReadDataAvailable == 0) {
      // Pipe has been drained. Reset suppression and re-arm sentinel
      // so the next write triggers a new edge notification.
      reg.pipe_et_suppressed = false;
      issue_sentinel_read(&reg);
    }
  }
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// internal::epoll_create1
//===----------------------------------------------------------------------===//

namespace internal {

intptr_t epoll_create1(int flags) {
  if (flags & ~EPOLL_CLOEXEC)
    return -EINVAL;

  // Ensure the completion router is installed with the reactor.
  ensure_router_installed();

  // Create a manual-reset event for per-instance ready signaling.
  HANDLE ready_event = nullptr;
  auto evt_oa = windows::internal_oa();
  NTSTATUS s = NtCreateEvent(&ready_event, EVENT_MODIFY_STATE | SYNCHRONIZE,
                             &evt_oa,
                             NotificationEvent, FALSE);
  if (!NT_SUCCESS(s))
    return -ENOMEM;
  auto close_event = cpp::make_scope_guard([&] { NtClose(ready_event); });

  // Per-instance reserve for guaranteed-delivery posts (synthetic
  // completions for regular files).
  HANDLE reserve = nullptr;
  auto rsv_oa = windows::internal_oa();
  s = NtAllocateReserveObject(&reserve, &rsv_oa, MemoryReserveIoCompletion);
  if (!NT_SUCCESS(s))
    return -ENOMEM;
  auto close_reserve = cpp::make_scope_guard([&] { NtClose(reserve); });

  // Allocate the epoll instance state.
  auto *inst = internal::epoll_instance_alloc();
  if (!inst)
    return -ENOMEM;
  auto free_inst =
      cpp::make_scope_guard([&] { internal::epoll_instance_free(inst); });

  inst->ready_event = ready_event;
  inst->reserve = reserve;

  // Allocate an fd for the epoll instance. The ready_event is the
  // underlying NT handle -- closeable via close(fd).
  auto result = internal::fd_table.alloc(ready_event, O_RDWR, 0,
                                          internal::FileKind::Epoll);
  if (!result)
    return -EMFILE;

  // All resources committed -- dismiss guards.
  free_inst.dismiss();
  close_reserve.dismiss();
  close_event.dismiss();

  // Store the epoll instance in the OFD aux union.
  auto *ofd = internal::fd_table.get_ofd(result.value());
  ofd->set_epoll_inst(inst);

  if (flags & EPOLL_CLOEXEC)
    internal::fd_table.set_fd_cloexec(result.value(), true);

  return result.value();
}

//===----------------------------------------------------------------------===//
// internal::epoll_ctl
//===----------------------------------------------------------------------===//

intptr_t epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
  auto *ep_ofd = internal::fd_table.get_ofd(epfd);
  if (!ep_ofd || !ep_ofd->is_epoll())
    return -EBADF;

  auto *inst = ep_ofd->epoll_inst();

  auto *target_ofd = internal::fd_table.get_ofd(fd);
  if (!target_ofd)
    return -EBADF;

  // O_PATH descriptors cannot be monitored for I/O events.
  // Linux returns EPERM for this case (not EBADF).
  if (target_ofd->is_path_only())
    return -EPERM;

  inst->lock.lock();

  switch (op) {
  case EPOLL_CTL_ADD: {
    if (internal::epoll_find(inst, fd)) {
      inst->lock.unlock();
      return -EEXIST;
    }

    auto *reg = internal::epoll_insert(inst, fd);
    if (!reg) {
      inst->lock.unlock();
      return -ENOMEM;
    }

    // Capture the fd's current generation for ABA detection. The
    // completion router validates this against the slot's live generation
    // to silently drop completions that arrive after the fd was closed
    // and the slot reused for a different file.
    auto *target_slot = internal::fd_table.get_slot(fd);
    internal::TaggedOfd tagged =
        target_slot->load_tagged(cpp::MemoryOrder::ACQUIRE);

    // Close the TOCTOU window between the get_ofd() above and this
    // generation read: if another thread closed and reopened the fd,
    // the pointer or generation will have changed.
    if (tagged.ptr() != target_ofd) {
      internal::epoll_remove(inst, fd);
      inst->lock.unlock();
      return -EBADF;
    }

    reg->events = event->events;
    reg->data = event->data.u64;
    reg->generation = tagged.gen();
    reg->wcp_in = nullptr;
    reg->wcp_out = nullptr;
    // Back-pointer for drain thread routing. RELEASE ensures the
    // instance pointer is visible to the router after all other fields.
    reg->instance.store(inst, cpp::MemoryOrder::RELEASE);

    HANDLE h = target_ofd->handle;

    // Inotify fd: monitor the readable_event via WCP (read-only).
    if (target_ofd->is_inotify()) {
      reg->is_afd = false;
      if ((event->events & (EPOLLIN | EPOLLRDNORM))) {
        if (register_wcp(h, reg, &reg->wcp_in) < 0) {
          internal::epoll_remove(inst, fd);
          inst->lock.unlock();
          return -ENOMEM;
        }
      }
    }
    // AFD socket: register via AFD_NOTIFY -> reactor IOCP.
    else if (target_ofd->kind == internal::FileKind::AfdSocket) {
      reg->is_afd = true;
      if (register_afd(h, reg) < 0) {
        internal::epoll_remove(inst, fd);
        inst->lock.unlock();
        return -EINVAL;
      }
    }
    // Socketpair/FIFO: bridge events via WCP -> reactor IOCP.
    // FifoChannel carries both readable_event and writable_event;
    // socketpairs have separate read/write channels.
    else if (target_ofd->kind == internal::FileKind::Fifo ||
             target_ofd->kind == internal::FileKind::SocketPair) {
      reg->is_afd = false;
      internal::FifoChannel *read_ch = nullptr;
      internal::FifoChannel *write_ch = nullptr;

      if (target_ofd->kind == internal::FileKind::Fifo) {
        auto *fifo = target_ofd->fifo_channel();
        read_ch = fifo;
        write_ch = fifo; // Same channel — writable_event lives here too.
      } else {
        auto *sp = target_ofd->socket_pair();
        read_ch = sp->read_ch;
        write_ch = sp->write_ch;
      }

      if ((event->events & (EPOLLIN | EPOLLRDNORM)) && read_ch) {
        if (register_wcp(read_ch->readable_event, reg, &reg->wcp_in) < 0) {
          internal::epoll_remove(inst, fd);
          inst->lock.unlock();
          return -ENOMEM;
        }
      }
      if ((event->events & (EPOLLOUT | EPOLLWRNORM)) && write_ch) {
        if (register_wcp(write_ch->writable_event, reg, &reg->wcp_out) < 0) {
          internal::epoll_remove(inst, fd);
          inst->lock.unlock();
          return -ENOMEM;
        }
      }
    }
    // Anonymous pipe: sentinel zero-byte read on a message-mode duplicate.
    // The sentinel pends until data arrives, then completes via IOCP
    // without consuming data.
    else if (target_ofd->is_pipe()) {
      reg->is_afd = false;
      reg->is_pipe = true;
      reg->pipe_et_suppressed = false;
      if (setup_pipe_sentinel(reg, h) < 0) {
        internal::epoll_remove(inst, fd);
        inst->lock.unlock();
        return -ENOMEM;
      }
    }
    // Regular file: always ready -- post synthetic completion.
    else {
      reg->is_afd = false;
      NtSetIoCompletionEx(reactor::iocp_handle(), inst->reserve,
                          reinterpret_cast<PVOID>(reg), nullptr,
                          STATUS_SUCCESS, 0);
    }
    break;
  }

  case EPOLL_CTL_DEL: {
    auto *reg = internal::epoll_find(inst, fd);
    if (!reg) {
      inst->lock.unlock();
      return -ENOENT;
    }

    // Deregister AFD_NOTIFY.
    if (reg->is_afd && target_ofd->kind == internal::FileKind::AfdSocket) {
      HANDLE h = target_ofd->handle;
      SOCK_NOTIFY_REGISTRATION snr = {};
      snr.Socket = h;
      snr.Operation = SOCK_NOTIFY_OP_REMOVE;

      AFD_NOTIFY_INPUT ni = {};
      ni.CompletionPort = reactor::iocp_handle();
      ni.Registrations = &snr;
      ni.RegistrationCount = 1;

      IO_STATUS_BLOCK iosb = {};
      NtDeviceIoControlFile(h, nullptr, nullptr, nullptr, &iosb,
                            IOCTL_AFD_NOTIFY, &ni, sizeof(ni), nullptr, 0);
    }

    internal::epoll_remove(inst, fd);
    break;
  }

  case EPOLL_CTL_MOD: {
    auto *reg = internal::epoll_find(inst, fd);
    if (!reg) {
      inst->lock.unlock();
      return -ENOENT;
    }

    // Refresh generation: the fd may have been recycled between the
    // original ADD and this MOD. If the OFD pointer changed, the fd
    // was closed and reopened — reject as stale.
    auto *mod_slot = internal::fd_table.get_slot(fd);
    internal::TaggedOfd mod_tagged =
        mod_slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
    if (mod_tagged.ptr() != target_ofd) {
      inst->lock.unlock();
      return -EBADF;
    }
    reg->generation = mod_tagged.gen();

    // Inotify fd: cancel + re-register WCP with new events.
    if (target_ofd->is_inotify()) {
      if (reg->wcp_in) {
        NtCancelWaitCompletionPacket(reg->wcp_in, TRUE);
        NtClose(reg->wcp_in);
        reg->wcp_in = nullptr;
      }
      reg->events = event->events;
      reg->data = event->data.u64;
      if ((event->events & (EPOLLIN | EPOLLRDNORM))) {
        if (register_wcp(target_ofd->handle, reg, &reg->wcp_in) < 0) {
          inst->lock.unlock();
          return -ENOMEM;
        }
      }
    }
    // For AFD sockets: disable + re-enable with new events.
    else if (reg->is_afd && target_ofd->kind == internal::FileKind::AfdSocket) {
      HANDLE h = target_ofd->handle;

      SOCK_NOTIFY_REGISTRATION snr = {};
      snr.Socket = h;
      snr.Operation = SOCK_NOTIFY_OP_DISABLE;
      AFD_NOTIFY_INPUT ni = {};
      ni.CompletionPort = reactor::iocp_handle();
      ni.Registrations = &snr;
      ni.RegistrationCount = 1;
      IO_STATUS_BLOCK iosb = {};
      NtDeviceIoControlFile(h, nullptr, nullptr, nullptr, &iosb,
                            IOCTL_AFD_NOTIFY, &ni, sizeof(ni), nullptr, 0);

      reg->events = event->events;
      reg->data = event->data.u64;
      if (register_afd(h, reg) < 0) {
        inst->lock.unlock();
        return -EINVAL;
      }
    }
    // For WCP-bridged fds: cancel + re-associate.
    else {
      if (reg->wcp_in) {
        NtCancelWaitCompletionPacket(reg->wcp_in, TRUE);
        NtClose(reg->wcp_in);
        reg->wcp_in = nullptr;
      }
      if (reg->wcp_out) {
        NtCancelWaitCompletionPacket(reg->wcp_out, TRUE);
        NtClose(reg->wcp_out);
        reg->wcp_out = nullptr;
      }

      reg->events = event->events;
      reg->data = event->data.u64;

      internal::FifoChannel *read_ch = nullptr;
      internal::FifoChannel *write_ch = nullptr;
      if (target_ofd->kind == internal::FileKind::Fifo) {
        auto *fifo = target_ofd->fifo_channel();
        read_ch = fifo;
        write_ch = fifo;
      } else if (target_ofd->kind == internal::FileKind::SocketPair) {
        auto *sp = target_ofd->socket_pair();
        read_ch = sp->read_ch;
        write_ch = sp->write_ch;
      }

      if ((event->events & (EPOLLIN | EPOLLRDNORM)) && read_ch)
        register_wcp(read_ch->readable_event, reg, &reg->wcp_in);
      if ((event->events & (EPOLLOUT | EPOLLWRNORM)) && write_ch)
        register_wcp(write_ch->writable_event, reg, &reg->wcp_out);
    }
    break;
  }

  default:
    inst->lock.unlock();
    return -EINVAL;
  }

  inst->lock.unlock();
  return 0;
}

//===----------------------------------------------------------------------===//
// internal::epoll_wait
//===----------------------------------------------------------------------===//

intptr_t epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                int timeout) {
  if (maxevents <= 0)
    return -EINVAL;

  auto *ep_ofd = internal::fd_table.get_ofd(epfd);
  if (!ep_ofd || !ep_ofd->is_epoll())
    return -EBADF;

  auto *inst = ep_ofd->epoll_inst();

  // Re-arm any ET-suppressed pipe sentinels that have since drained.
  rearm_suppressed_pipes(inst);

  // Phase 1: Non-blocking flush of the shared IOCP.
  flush_shared_iocp();

  // Phase 2: Drain pending queue into the events array.
  int result = 0;
  {
    inst->pending_lock.lock();
    result = drain_pending(inst, events, maxevents, result);
    if (inst->pending_head == inst->pending_tail)
      ::NtClearEvent(inst->ready_event);
    inst->pending_lock.unlock();
    // Wake any router thread parked on a full pending queue.
    inst->drain_seq.fetch_add(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&inst->drain_seq, 1);
  }

  // If we already have events, or timeout=0 (non-blocking), return now.
  if (result > 0 || timeout == 0)
    return result;

  // Phase 3: Blocking wait on per-instance ready_event.
  // Pipe sentinel reads complete via the reactor IOCP → drain thread →
  // pending queue → ready_event, so no special pipe handling needed.
  LARGE_INTEGER nt_timeout;
  LARGE_INTEGER *nt_timeout_ptr = nullptr;
  if (timeout >= 0) {
    nt_timeout.QuadPart = -static_cast<int64_t>(timeout) * 10000LL;
    nt_timeout_ptr = &nt_timeout;
  }

  NTSTATUS s = ::NtWaitForSingleObject(inst->ready_event,
                                       /*Alertable=*/TRUE, nt_timeout_ptr);

  if (s == STATUS_TIMEOUT)
    return 0;

  if (s == STATUS_USER_APC || s == STATUS_ALERTED)
    return -EINTR;

  // Phase 4: Flush again and drain pending queue.
  flush_shared_iocp();

  {
    inst->pending_lock.lock();
    result = drain_pending(inst, events, maxevents, result);
    if (inst->pending_head == inst->pending_tail)
      ::NtClearEvent(inst->ready_event);
    inst->pending_lock.unlock();
    // Wake any router thread parked on a full pending queue.
    inst->drain_seq.fetch_add(1, cpp::MemoryOrder::RELEASE);
    futex_addr::wake(&inst->drain_seq, 1);
  }

  return result;
}

bool epoll_release_aux(OpenFileDescription *ofd) {
  auto *inst = ofd->epoll_inst();

  // Phase 0: Deregister AFD socket subscriptions. Unlike WCPs and
  // sentinel reads (which are cancelled in epoll_instance_shutdown),
  // AFD_NOTIFY registrations are persistent — the kernel keeps posting
  // completions with our EpollRegistration* as the key until we
  // explicitly remove them. Without this, AFD completions would arrive
  // on the IOCP after we free the registration memory.
  //
  // We must do this here (not in socket_epoll.h) because it requires
  // fd_table access to look up the target socket handle.
  inst->lock.lock();
  for (uint32_t i = 0; i < inst->capacity; ++i) {
    auto &reg = inst->regs[i];
    if (reg.fd < 0 || !reg.is_afd)
      continue;

    // Look up the target fd's OFD to get the socket handle. If the fd
    // was already closed, the AFD registration was implicitly removed
    // by the kernel when the socket handle was closed — skip.
    auto *target_ofd = fd_table.get_ofd(reg.fd);
    if (!target_ofd || target_ofd->kind != FileKind::AfdSocket)
      continue;

    // Validate generation: if the fd was recycled, this registration
    // is stale and the AFD subscription was already removed when the
    // original socket was closed.
    auto *slot = fd_table.get_slot(reg.fd);
    if (slot &&
        slot->load_tagged(cpp::MemoryOrder::RELAXED).gen() != reg.generation)
      continue;

    SOCK_NOTIFY_REGISTRATION snr = {};
    snr.Socket = target_ofd->handle;
    snr.Operation = SOCK_NOTIFY_OP_REMOVE;

    AFD_NOTIFY_INPUT ni = {};
    ni.CompletionPort = reactor::iocp_handle();
    ni.Registrations = &snr;
    ni.RegistrationCount = 1;

    IO_STATUS_BLOCK iosb = {};
    NtDeviceIoControlFile(target_ofd->handle, nullptr, nullptr, nullptr,
                          &iosb, IOCTL_AFD_NOTIFY, &ni, sizeof(ni),
                          nullptr, 0);
  }
  inst->lock.unlock();

  // Phase 1: Tombstone all registrations and cancel outstanding I/O.
  // After this, any router invocation for this instance sees
  // reg->instance == nullptr (ACQUIRE load) and bails immediately.
  epoll_instance_shutdown(inst);

  // Phase 2: Fence — wait for all in-flight completions to be processed.
  // After the tombstone above, no NEW router invocation can access this
  // instance (it sees nullptr and bails). We need to wait for:
  //
  //   (a) The drain thread to finish any batch it dequeued before the
  //       tombstone — fence_drain_cycle() covers this by waiting for
  //       the drain thread to complete a full cycle.
  //
  //   (b) All flush threads (epoll_wait callers doing flush_shared_iocp)
  //       to finish processing — g_router_active == 0 covers this.
  //       flush_shared_iocp() brackets its entire dequeue+dispatch loop
  //       with g_router_active, so the counter is > 0 while any flush
  //       thread holds completion keys on its stack.
  //
  // Order matters: fence first (covers drain thread's dequeue-to-router
  // gap), then activity check (covers flush threads). After both, no
  // thread can hold a pointer into regs[] or inst.

  // Wait for the drain thread to complete one full cycle.
  internal::reactor::fence_drain_cycle();

  // Wait for all flush threads and in-flight router calls to finish.
  // Park on the g_router_active counter via futex instead of busy-spinning.
  // The router exit path wakes us when the counter transitions to zero.
  {
    LARGE_INTEGER fence_timeout;
    fence_timeout.QuadPart = -500000; // 50ms safety net
    for (;;) {
      uint32_t val = g_router_active.load(cpp::MemoryOrder::ACQUIRE);
      if (val == 0)
        break;
      futex_addr::wait_nt(&g_router_active, val, &fence_timeout);
    }
  }

  // Phase 3: Now safe to free — no thread holds a pointer into regs[]
  // or inst.
  epoll_instance_free_pages(inst);

  return true;
}

// ---------------------------------------------------------------------------
// Fork reinit: invalidate all epoll instances after reactor rebuild.
//
// After fork the reactor IOCP is brand new. All WCP registrations,
// AFD_NOTIFY subscriptions, and sentinel reads targeted the parent's
// IOCP and are dead. We walk every live fd, find epoll instances, and:
//   1. Close stale WCP/sentinel handles per registration.
//   2. Close the stale ready_event and reserve handles.
//   3. Recreate ready_event and reserve for the child.
//   4. Reset locks and clear the pending queue.
//   5. Clear all registrations — they'll be re-armed by epoll_ctl.
// ---------------------------------------------------------------------------

static void fork_reinit_instance(EpollInstance *inst) {
  // All per-registration handles (WCPs, sentinel handles) and per-instance
  // handles (ready_event, reserve) were created with internal_oa()
  // (non-inheritable), so they don't exist in the child's handle table.
  // Just null pointers and clear slots — no NtClose.
  for (uint32_t i = 0; i < inst->capacity; ++i) {
    auto &reg = inst->regs[i];
    if (reg.fd < 0)
      continue;
    // Clear the slot — the fd still exists but the epoll registration
    // must be re-armed by user code via epoll_ctl after fork.
    // Use epoll_reg_clear instead of memset to properly handle the
    // atomic instance field.
    epoll_reg_clear(&reg);
  }
  inst->count = 0;

  // Recreate the per-instance ready_event (stale pointer is non-inherited).
  inst->ready_event = nullptr;
  {
    auto oa = windows::internal_oa();
    ::NtCreateEvent(&inst->ready_event, EVENT_ALL_ACCESS, &oa,
                    NotificationEvent, FALSE);
  }

  // Recreate the per-instance reserve object (stale pointer is non-inherited).
  inst->reserve = nullptr;
  auto reinit_rsv_oa = windows::internal_oa();
  ::NtAllocateReserveObject(&inst->reserve, &reinit_rsv_oa,
                            MemoryReserveIoCompletion);

  // Reset locks (clears both value and stale Treiber wait stack).
  inst->lock.reset_for_fork();
  inst->pending_lock.reset_for_fork();

  // Flush the pending queue.
  inst->pending_head = 0;
  inst->pending_tail = 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

void LIBC_NAMESPACE::internal::epoll_fork_reinit() {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::internal;
  // Walk all live fds and reinit any epoll instances.
  fd_table.for_each_live(
      [](int /*fd*/, FdSlot *slot, void * /*ctx*/) {
        auto *ofd = slot->load_ofd(cpp::MemoryOrder::RELAXED);
        if (ofd && ofd->is_epoll())
          fork_reinit_instance(ofd->epoll_inst());
      },
      nullptr);
}
