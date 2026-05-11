//===-- Windows poll engine (IOCP + WCP based) ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Readiness model:
//   - AFD sockets: batch IOCTL_AFD_POLL with dynamically-sized buffer.
//   - Socketpairs/FIFOs: ring-buffer position checks with waitable events.
//   - Console/pipe handles: the HANDLE itself is waitable.
//   - Disk files: always ready (POSIX mandates this for regular files).
//
// Wait strategy — single path, no arbitrary limits:
//   A per-call IOCP + WaitCompletionPackets bridge every waitable handle
//   to one unified wait. The same NT primitive the process-wide reactor
//   uses, but with a call-private IOCP — zero shared state, zero
//   contention, self-contained cleanup.
//
//   For mixed AFD-socket + waitable-handle sets, AFD_POLL is issued async
//   with an event. A WCP watches that event on the same per-call IOCP,
//   giving correct single-timeout semantics for any fd mix.
//
//   No NtWaitForMultipleObjects, no 64-handle limit, no dual paths.
//
// ppoll is the primary engine. poll() converts ms → timespec and delegates.
// Signal mask atomicity follows the sigsuspend pattern: save → install →
// alertable wait → restore.
//
//===----------------------------------------------------------------------===//

#include "poll_ops.h"

#include "hdr/types/nfds_t.h"
#include "hdr/types/struct_pollfd.h"
#include "hdr/types/struct_timespec.h"
#include "include/llvm-libc-macros/poll-macros.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/threads/windows/spin_wait.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/pthread/cancel_internal.h"

#include <errno.h>

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// Shared \Device\Afd\Mio handle for AFD_POLL (lazy, process-lifetime)
//===----------------------------------------------------------------------===//

static cpp::Atomic<HANDLE> g_afd_mio{nullptr};

static HANDLE get_afd_mio() {
  HANDLE h = g_afd_mio.load(cpp::MemoryOrder::ACQUIRE);
  if (h)
    return h;

  static const WCHAR path[] = u"\\Device\\Afd\\Mio";
  windows::nt_wstring_view name(path);

  auto oa = windows::named_internal_oa(&name);

  windows::ScopedNtHandle mio;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = NtCreateFile(mio.put(), SYNCHRONIZE, &oa, &iosb, nullptr, 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0,
                            nullptr, 0);
  if (!NT_SUCCESS(s))
    return nullptr;

  HANDLE expected = nullptr;
  if (g_afd_mio.compare_exchange_strong(expected, mio.get(),
                                        cpp::MemoryOrder::ACQ_REL))
    (void)mio.release(); // CAS won — ownership transferred to atomic.
  // CAS lost — destructor closes the duplicate.
  return g_afd_mio.load(cpp::MemoryOrder::ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Translate POSIX poll events <-> AFD poll events
//===----------------------------------------------------------------------===//

static ULONG posix_to_afd_events(short events,
                                 internal::SocketState *state) {
  ULONG afd = AFD_POLL_LOCAL_CLOSE;
  if (events & (POLLIN | POLLRDNORM))
    afd |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT;
  if (events & (POLLOUT | POLLWRNORM))
    afd |= AFD_POLL_SEND;
  if (events & POLLPRI)
    afd |= AFD_POLL_RECEIVE_EXPEDITED;
  // POLLRDHUP detects peer shutdown(SHUT_WR) — AFD reports this as
  // AFD_POLL_DISCONNECT. Always request it so we can report the
  // distinction between half-close (POLLRDHUP) and full close (POLLHUP).
  if (events & POLLRDHUP)
    afd |= AFD_POLL_DISCONNECT;
  if (state &&
      state->phase.load(cpp::MemoryOrder::ACQUIRE) ==
          internal::SocketPhase::CONNECTING) {
    afd |= AFD_POLL_CONNECT_FAIL;
    if (events & (POLLOUT | POLLWRNORM))
      afd |= AFD_POLL_CONNECT;
  }
  return afd;
}

static short afd_to_posix_events(ULONG afd, short requested) {
  short rev = 0;
  if (afd & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT))
    rev |= requested & (POLLIN | POLLRDNORM);
  if (afd & AFD_POLL_SEND)
    rev |= requested & (POLLOUT | POLLWRNORM);
  if (afd & AFD_POLL_CONNECT)
    rev |= requested & (POLLOUT | POLLWRNORM);
  // AFD_POLL_DISCONNECT = peer called shutdown(SHUT_WR). This is a
  // half-close: the read side sees EOF but the write side may still work.
  // Report POLLRDHUP (if requested) and POLLIN (EOF is readable).
  // Do NOT report POLLHUP here — AFD_POLL_SEND presence in the result
  // depends on whether it was *requested*, not on socket state, so we
  // can't reliably detect full close from a single poll result.
  // POLLHUP for full close comes via AFD_POLL_ABORT or LOCAL_CLOSE.
  if (afd & AFD_POLL_DISCONNECT) {
    rev |= requested & POLLRDHUP;
    rev |= requested & (POLLIN | POLLRDNORM);
  }
  // AFD_POLL_ABORT = connection reset / abortive close. Both directions
  // are dead — report POLLERR, POLLHUP, and POLLRDHUP (recv is dead).
  if (afd & AFD_POLL_ABORT) {
    rev |= POLLERR | POLLHUP;
    rev |= requested & POLLRDHUP;
  }
  if (afd & AFD_POLL_CONNECT_FAIL)
    rev |= POLLERR;
  return rev;
}

//===----------------------------------------------------------------------===//
// Timeout conversion: timespec → NT 100ns relative ticks
//===----------------------------------------------------------------------===//

static LARGE_INTEGER *timespec_to_nt(const struct timespec *tmo,
                                     LARGE_INTEGER *nt_out) {
  if (!tmo)
    return nullptr;
  nt_out->QuadPart = -(static_cast<int64_t>(tmo->tv_sec) * 10000000LL +
                       static_cast<int64_t>(tmo->tv_nsec) / 100);
  return nt_out;
}

//===----------------------------------------------------------------------===//
// RAII guard: restore signal mask on all exit paths
//===----------------------------------------------------------------------===//

namespace {
struct SigmaskGuard {
  bool active = false;
  sigset_t saved_mask;

  ~SigmaskGuard() {
    if (active)
      (void)signal_state::rt_sigprocmask(SIG_SETMASK, &saved_mask, nullptr);
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// Process AFD_POLL results into pollfd revents
//===----------------------------------------------------------------------===//

static int process_afd_results(uint8_t *poll_buf,
                               internal::SocketState **afd_state, int *afd_idx,
                               int afd_count, struct pollfd *fds) {
  auto *poll_info = reinterpret_cast<AFD_POLL_INFO *>(poll_buf);
  int ready = 0;
  for (int j = 0; j < afd_count; ++j) {
    if (afd_state[j]->phase.load(cpp::MemoryOrder::ACQUIRE) ==
            internal::SocketPhase::CONNECTING &&
        (poll_info->Handles[j].PollEvents &
         (AFD_POLL_CONNECT | AFD_POLL_CONNECT_FAIL)))
      internal::socket_finalize_connect(afd_state[j],
                                        poll_info->Handles[j].Status);
    short rev = afd_to_posix_events(poll_info->Handles[j].PollEvents,
                                    fds[afd_idx[j]].events);
    if (rev) {
      fds[afd_idx[j]].revents |= rev;
      ++ready;
    }
  }
  return ready;
}

namespace internal {

//===----------------------------------------------------------------------===//
// ppoll — primary engine
//===----------------------------------------------------------------------===//

intptr_t ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo,
               const sigset_t *sigmask) {
  // ─── Phase 0: Timeout + signal mask ───────────────────────────────
  LARGE_INTEGER nt_timeout;
  LARGE_INTEGER *nt_timeout_ptr = timespec_to_nt(tmo, &nt_timeout);
  bool is_nonblocking = nt_timeout_ptr && nt_timeout_ptr->QuadPart == 0;

  SigmaskGuard guard;
  if (sigmask) {
    signal_state::ThreadSignalState *sig_state =
        signal_state::get_thread_state();
    if (!sig_state)
      return -EAGAIN;

    sig_state->handler_ran = false;
    (void)signal_state::rt_sigprocmask(SIG_SETMASK, sigmask, &guard.saved_mask);
    guard.active = true;
    if (sig_state->handler_ran)
      return -EINTR;
  }

  // ─── Dynamic workspace allocation ─────────────────────────────────
  //
  // All arrays sized to nfds (upper bound for any single category).
  // wait arrays get +1 for the AFD event slot in mixed mode.
  //
  // Layout (all 8-byte aligned):
  //   afd_handles[n]  — AFD_POLL_HANDLE_INFO per socket
  //   afd_idx[n]      — maps AFD index → pollfd index
  //   afd_state[n]    — SocketState* per socket
  //   wait_handles[n1] — original HANDLE per waitable fd (+1 AFD event)
  //   wait_to_idx[n1] — maps wait slot → pollfd index
  //   wait_events[n1] — POSIX event mask per wait slot (POLLIN or POLLOUT)
  //   wcp_handles[n1] — WaitCompletionPacket per wait slot
  //   poll_buf        — AFD_POLL_INFO + n * AFD_POLL_HANDLE_INFO
  //   iosb            — IO_STATUS_BLOCK for AFD_POLL
  auto align8 = [](size_t s) -> size_t { return (s + 7) & ~size_t{7}; };

  size_t n = static_cast<size_t>(nfds);
  // Each fd can contribute up to 2 wait slots (read + write for
  // socketpairs/FIFOs), plus 1 slot for the AFD event.
  size_t n1 = 2 * n + 1;

  size_t afd_handles_sz = align8(n * sizeof(AFD_POLL_HANDLE_INFO));
  size_t afd_idx_sz = align8(n * sizeof(int));
  size_t afd_state_sz = align8(n * sizeof(internal::SocketState *));
  size_t wait_hdl_sz = align8(n1 * sizeof(HANDLE));
  size_t wait_idx_sz = align8(n1 * sizeof(int));
  size_t wait_evt_sz = align8(n1 * sizeof(short));
  size_t wcp_hdl_sz = align8(n1 * sizeof(HANDLE));
  size_t poll_buf_sz =
      align8(sizeof(AFD_POLL_INFO) + n * sizeof(AFD_POLL_HANDLE_INFO));
  size_t iosb_sz = align8(sizeof(IO_STATUS_BLOCK));

  size_t total = afd_handles_sz + afd_idx_sz + afd_state_sz + wait_hdl_sz +
                 wait_idx_sz + wait_evt_sz + wcp_hdl_sz + poll_buf_sz +
                 iosb_sz;
  if (total == 0)
    total = 64;

  auto *raw = static_cast<uint8_t *>(internal::page_alloc(total));
  if (!raw)
    return -ENOMEM;
  __builtin_memset(raw, 0, total);
  auto free_ws = cpp::make_scope_guard([&] { internal::page_free(raw); });

  // Lay out arrays sequentially in the allocation.
  uint8_t *p = raw;
  auto *ws_afd_handles = reinterpret_cast<AFD_POLL_HANDLE_INFO *>(p);
  p += afd_handles_sz;
  auto *ws_afd_idx = reinterpret_cast<int *>(p);
  p += afd_idx_sz;
  auto *ws_afd_state = reinterpret_cast<internal::SocketState **>(p);
  p += afd_state_sz;
  auto *ws_wait_handles = reinterpret_cast<HANDLE *>(p);
  p += wait_hdl_sz;
  auto *ws_wait_to_idx = reinterpret_cast<int *>(p);
  p += wait_idx_sz;
  auto *ws_wait_events = reinterpret_cast<short *>(p);
  p += wait_evt_sz;
  auto *ws_wcp_handles = reinterpret_cast<HANDLE *>(p);
  p += wcp_hdl_sz;
  auto *ws_poll_buf = p;
  p += poll_buf_sz;
  auto *ws_iosb = reinterpret_cast<IO_STATUS_BLOCK *>(p);

  // ─── Phase 1: Classify each fd ────────────────────────────────────
  //
  // Sorts every fd into one of:
  //   1. Immediately ready → increment ready count
  //   2. AFD socket → add to afd_handles batch
  //   3. Waitable handle → add to wait_handles + wait_events
  //
  // No caps. Arrays are sized to nfds.
  int wait_count = 0;
  int ready = 0;
  int afd_count = 0;

  for (nfds_t i = 0; i < nfds; ++i) {
    fds[i].revents = 0;

    if (fds[i].fd < 0)
      continue;

    internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fds[i].fd);
    if (!ofd) {
      fds[i].revents = POLLNVAL;
      ++ready;
      continue;
    }

    // AFD socket: batch into IOCTL_AFD_POLL.
    if (ofd->kind == internal::FileKind::AfdSocket) {
      ws_afd_handles[afd_count].Handle = ofd->handle;
      ws_afd_handles[afd_count].PollEvents =
          posix_to_afd_events(fds[i].events, ofd->afd_socket());
      ws_afd_handles[afd_count].Status = 0;
      ws_afd_idx[afd_count] = static_cast<int>(i);
      ws_afd_state[afd_count] = ofd->afd_socket();
      ++afd_count;
      continue;
    }

    // Socketpair: bidirectional ring buffer.
    if (ofd->kind == internal::FileKind::SocketPair) {
      auto *sp = ofd->socket_pair();

      // Read channel.
      internal::FifoHeader *rd_hdr = sp->read_ch->header();
      uint64_t rd_rp = rd_hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
      uint64_t rd_wp = rd_hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
      bool rd_no_writers =
          rd_hdr->writer_count.load(cpp::MemoryOrder::ACQUIRE) == 0;

      if (rd_no_writers) {
        fds[i].revents |= POLLHUP;
        fds[i].revents |= fds[i].events & POLLRDHUP;
      }

      if (fds[i].events & (POLLIN | POLLRDNORM)) {
        if (rd_rp < rd_wp || rd_no_writers) {
          fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
        } else {
          ws_wait_handles[wait_count] = sp->read_ch->readable_event;
          ws_wait_to_idx[wait_count] = static_cast<int>(i);
          ws_wait_events[wait_count] = POLLIN | POLLRDNORM;
          ++wait_count;
        }
      }

      // Write channel.
      if (fds[i].events & (POLLOUT | POLLWRNORM)) {
        internal::FifoHeader *wr_hdr = sp->write_ch->header();
        uint64_t wr_rp = wr_hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
        uint64_t wr_wp = wr_hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
        bool wr_no_readers =
            wr_hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) == 0;
        size_t space = internal::FIFO_RING_CAPACITY -
                       static_cast<size_t>(wr_wp - wr_rp);
        if (space > internal::FIFO_RING_CAPACITY)
          space = 0;
        if (space > 0 || wr_no_readers) {
          fds[i].revents |= fds[i].events & (POLLOUT | POLLWRNORM);
        } else {
          ws_wait_handles[wait_count] = sp->write_ch->writable_event;
          ws_wait_to_idx[wait_count] = static_cast<int>(i);
          ws_wait_events[wait_count] = POLLOUT | POLLWRNORM;
          ++wait_count;
        }
      }

      if (fds[i].revents)
        ++ready;
      continue;
    }

    // FIFO / anonymous pipe with event-only FifoChannel.
    internal::FifoChannel *fifo_ch =
        ofd->kind == internal::FileKind::Fifo
            ? ofd->fifo_channel()
            : ofd->pipe_events_or_null();
    if (fifo_ch) {
      internal::FifoHeader *hdr = fifo_ch->header();

      if (hdr) {
        // Ring-buffer-backed FIFO: check positions directly.
        uint64_t rp = hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
        uint64_t wp = hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
        bool no_writers =
            hdr->writer_count.load(cpp::MemoryOrder::ACQUIRE) == 0;
        bool no_readers =
            hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) == 0;

        if (no_writers) {
          fds[i].revents |= POLLHUP;
          fds[i].revents |= fds[i].events & POLLRDHUP;
        }
        if (no_readers)
          fds[i].revents |= POLLERR;

        if (fds[i].events & (POLLIN | POLLRDNORM)) {
          if (rp < wp || no_writers) {
            fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
          } else {
            ws_wait_handles[wait_count] = fifo_ch->readable_event;
            ws_wait_to_idx[wait_count] = static_cast<int>(i);
            ws_wait_events[wait_count] = POLLIN | POLLRDNORM;
            ++wait_count;
          }
        }
        if (fds[i].events & (POLLOUT | POLLWRNORM)) {
          size_t space =
              internal::FIFO_RING_CAPACITY - static_cast<size_t>(wp - rp);
          if (space > internal::FIFO_RING_CAPACITY)
            space = 0;
          if (space > 0 || no_readers) {
            fds[i].revents |= fds[i].events & (POLLOUT | POLLWRNORM);
          } else {
            ws_wait_handles[wait_count] = fifo_ch->writable_event;
            ws_wait_to_idx[wait_count] = static_cast<int>(i);
            ws_wait_events[wait_count] = POLLOUT | POLLWRNORM;
            ++wait_count;
          }
        }
      } else {
        // Event-only FifoChannel (anonymous pipe): check pipe state
        // first for immediate readiness, then fall back to event wait.
        IO_STATUS_BLOCK pi_iosb = {};
        FILE_PIPE_LOCAL_INFORMATION pi = {};
        NTSTATUS ps = ::NtQueryInformationFile(
            ofd->handle, &pi_iosb, &pi, sizeof(pi),
            FilePipeLocalInformation);

        if (NT_SUCCESS(ps) &&
            pi.NamedPipeState >= FILE_PIPE_CLOSING_STATE) {
          fds[i].revents |= POLLHUP;
          fds[i].revents |= fds[i].events & POLLRDHUP;
        } else if (NT_SUCCESS(ps) && pi.ReadDataAvailable > 0) {
          if (fds[i].events & (POLLIN | POLLRDNORM))
            fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
        } else if (fds[i].events & (POLLIN | POLLRDNORM)) {
          // No data, pipe still connected — wait on readable_event.
          ws_wait_handles[wait_count] = fifo_ch->readable_event;
          ws_wait_to_idx[wait_count] = static_cast<int>(i);
          ws_wait_events[wait_count] = POLLIN | POLLRDNORM;
          ++wait_count;
        }
        if (fds[i].events & (POLLOUT | POLLWRNORM)) {
          fds[i].revents |= fds[i].events & (POLLOUT | POLLWRNORM);
        }
      }

      if (fds[i].revents)
        ++ready;
      continue;
    }

    // Console/disk/other: writes always ready, seekable reads always ready.
    if (fds[i].events & (POLLOUT | POLLWRNORM))
      fds[i].revents |= fds[i].events & (POLLOUT | POLLWRNORM);

    if (fds[i].events & (POLLIN | POLLRDNORM)) {
      if (ofd->is_seekable()) {
        fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
      } else {
        ws_wait_handles[wait_count] = ofd->handle;
        ws_wait_to_idx[wait_count] = static_cast<int>(i);
        ws_wait_events[wait_count] = POLLIN | POLLRDNORM;
        ++wait_count;
      }
    }

    if (fds[i].revents)
      ++ready;
  }

  // ─── Phase 2: Early return if anything is already ready ───────────
  if (ready > 0)
    return ready;

  // ─── Phase 3: Wait ────────────────────────────────────────────────
  //
  // Single strategy: per-call IOCP + WaitCompletionPackets.
  //
  // Every waitable handle gets a WCP → private IOCP. For mixed
  // AFD + waitable sets, AFD_POLL is issued async with an event,
  // and a WCP watches that event on the same IOCP. One wait point,
  // one timeout, any number of handles.
  bool have_afd = afd_count > 0;
  bool have_wait = wait_count > 0;

  // Nothing to wait on — alertable sleep.
  if (!have_afd && !have_wait) {
    if (is_nonblocking)
      return 0;
    NTSTATUS s = NtDelayExecution(/*Alertable=*/1, nt_timeout_ptr);
    if (s == STATUS_USER_APC) {
      cancel_check();
      signal_state::should_restart_syscall();
      return -EINTR;
    }
    return 0;
  }

  // ─── Issue async AFD_POLL if we have sockets ──────────────────────
  //
  // An event signals when AFD_POLL completes. That event is either
  // watched by a WCP (mixed mode) or waited on directly (AFD-only
  // with no waitable handles — still goes through the IOCP path
  // for uniformity).
  windows::ScopedNtHandle afd_event;
  HANDLE afd_mio = nullptr; // Saved mio handle for cancellation.
  bool afd_pending = false;
  int afd_wait_slot = -1;

  auto cleanup_afd = cpp::make_scope_guard([&] {
    if (afd_pending && afd_mio) {
      IO_STATUS_BLOCK cancel_iosb = {};
      ::NtCancelIoFileEx(afd_mio, ws_iosb, &cancel_iosb);
      // Wait for the kernel to complete the cancellation. No explicit
      // waker — the kernel writes to IOSB.Status directly. wait_nt's
      // internal 32-iteration spin catches fast completions; the timeout
      // handles slow ones.
      LARGE_INTEGER cancel_timeout;
      cancel_timeout.QuadPart = -100000; // 10ms per iteration
      while (*reinterpret_cast<volatile LONG *>(&ws_iosb->Status) ==
             static_cast<LONG>(STATUS_PENDING))
        futex_addr::wait_nt<int32_t>(
            reinterpret_cast<const volatile int32_t *>(&ws_iosb->Status),
            static_cast<int32_t>(STATUS_PENDING), &cancel_timeout);
    }
    // afd_event closed by ScopedNtHandle destructor.
  });

  if (have_afd) {
    afd_mio = get_afd_mio();
    if (afd_mio) {
      auto afd_evt_oa = windows::internal_oa();
      ::NtCreateEvent(afd_event.put(), EVENT_MODIFY_STATE | SYNCHRONIZE,
                      &afd_evt_oa, SynchronizationEvent, FALSE);
      if (afd_event) {
        auto *poll_info = reinterpret_cast<AFD_POLL_INFO *>(ws_poll_buf);
        if (!nt_timeout_ptr)
          poll_info->Timeout.QuadPart = 0x7FFFFFFFFFFFFFFFLL;
        else
          poll_info->Timeout.QuadPart = nt_timeout_ptr->QuadPart;
        poll_info->NumberOfHandles = static_cast<ULONG>(afd_count);
        poll_info->Unique = 0;
        __builtin_memcpy(poll_info->Handles, ws_afd_handles,
                         afd_count * sizeof(AFD_POLL_HANDLE_INFO));

        ULONG poll_size = static_cast<ULONG>(
            sizeof(AFD_POLL_INFO) +
            afd_count * sizeof(AFD_POLL_HANDLE_INFO));

        NTSTATUS s = ::NtDeviceIoControlFile(
            afd_mio, afd_event.get(), nullptr, nullptr, ws_iosb,
            IOCTL_AFD_POLL, ws_poll_buf, poll_size, ws_poll_buf, poll_size);

        if (s == STATUS_PENDING) {
          afd_pending = true;
        } else if (NT_SUCCESS(s)) {
          // AFD completed synchronously — event is already signaled.
          // We'll pick up results in Phase 4.
        } else if (s == STATUS_INVALID_HANDLE) {
          g_afd_mio.store(nullptr, cpp::MemoryOrder::RELEASE);
        }

        // Add AFD event as a waitable handle on the IOCP.
        afd_wait_slot = wait_count;
        ws_wait_handles[wait_count] = afd_event.get();
        ws_wait_to_idx[wait_count] = -1; // Sentinel: not a pollfd.
        ws_wait_events[wait_count] = 0;
        ++wait_count;
      }
    }
  }

  // ─── Create per-call IOCP ─────────────────────────────────────────
  windows::ScopedNtHandle poll_iocp;
  auto iocp_oa = windows::internal_oa();
  NTSTATUS s = ::NtCreateIoCompletion(poll_iocp.put(), IO_COMPLETION_ALL_ACCESS,
                                      &iocp_oa, 1);
  if (!NT_SUCCESS(s))
    return -ENOMEM;

  // ─── Create and associate WCPs ────────────────────────────────────
  //
  // One WCP per waitable handle (including the AFD event). When a
  // handle becomes signaled, the kernel posts a completion to our
  // private IOCP with KeyContext = slot index.
  int wcp_count = 0;
  auto cleanup_wcps = cpp::make_scope_guard([&] {
    for (int i = 0; i < wcp_count; ++i) {
      if (ws_wcp_handles[i]) {
        ::NtCancelWaitCompletionPacket(ws_wcp_handles[i], TRUE);
        ::NtClose(ws_wcp_handles[i]);
      }
    }
  });

  for (int i = 0; i < wait_count; ++i) {
    HANDLE wcp = nullptr;
    auto wcp_oa = windows::internal_oa();
    s = ::NtCreateWaitCompletionPacket(&wcp,
                                       WAIT_COMPLETION_PACKET_ALL_ACCESS,
                                       &wcp_oa);
    if (!NT_SUCCESS(s)) {
      ws_wcp_handles[wcp_count++] = nullptr;
      continue;
    }

    BOOLEAN already_signaled = FALSE;
    s = ::NtAssociateWaitCompletionPacket(
        wcp, poll_iocp.get(), ws_wait_handles[i],
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(i)), // Key = index
        nullptr, STATUS_SUCCESS, 0, &already_signaled);

    if (!NT_SUCCESS(s)) {
      ::NtClose(wcp);
      ws_wcp_handles[wcp_count++] = nullptr;
      continue;
    }

    ws_wcp_handles[wcp_count++] = wcp;
  }

  // ─── Unified alertable wait ───────────────────────────────────────
  //
  // One IOCP dequeue. Returns when any handle signals, timeout fires,
  // or an APC arrives (signal delivery → EINTR).
  FILE_IO_COMPLETION_INFORMATION completion = {};
  ULONG removed = 0;

  s = ::NtRemoveIoCompletionEx(poll_iocp.get(), &completion, 1, &removed,
                                nt_timeout_ptr, /*Alertable=*/1);

  if (s == STATUS_TIMEOUT)
    return 0;

  if (s == STATUS_USER_APC) {
    cancel_check();
    signal_state::should_restart_syscall();
    return -EINTR;
  }

  // ─── Phase 4: Probe all handles for readiness ─────────────────────
  //
  // At least one handle signaled (or AFD completed). Probe every
  // waitable handle with a zero-timeout check to catch additional
  // handles that became ready during the wait.
  LARGE_INTEGER zero_timeout = {};

  for (int i = 0; i < wait_count; ++i) {
    if (i == afd_wait_slot)
      continue; // AFD event — handled via IOSB below.
    if (::NtWaitForSingleObject(ws_wait_handles[i], /*Alertable=*/0,
                                &zero_timeout) == STATUS_SUCCESS) {
      int idx = ws_wait_to_idx[i];
      fds[idx].revents |= fds[idx].events & ws_wait_events[i];
      ++ready;
    }
  }

  // Process AFD results if the IOCTL completed (synchronously or async).
  if (have_afd && afd_event && NT_SUCCESS(ws_iosb->Status))
    ready += process_afd_results(ws_poll_buf, ws_afd_state, ws_afd_idx,
                                 afd_count, fds);

  return ready;
}

//===----------------------------------------------------------------------===//
// poll — thin wrapper over ppoll
//===----------------------------------------------------------------------===//

intptr_t poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  if (timeout < 0)
    return ppoll(fds, nfds, nullptr, nullptr);
  struct timespec ts;
  ts.tv_sec = timeout / 1000;
  ts.tv_nsec = static_cast<long>(timeout % 1000) * 1000000L;
  return ppoll(fds, nfds, &ts, nullptr);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
