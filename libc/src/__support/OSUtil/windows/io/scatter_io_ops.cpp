//===-- Internal readv/writev engine implementation ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scatter/gather I/O with full transport dispatch. Routes through the correct
// data path — FIFO ring buffers (pipe2/mkfifo), socketpair channels, AFD
// sockets, or IO Ring — mirroring the dispatch logic in read_write.cpp.
//
// For IO Ring paths (disk files, NT pipes, connected AFD sockets), readv and
// writev batch all iovec elements into a single NtSubmitIoRing syscall using
// BatchEngine. This collapses N kernel transitions into 1, with CQEs drained
// in an alertable loop for EINTR delivery.
//
// Non-IoRing transports (FIFO, socketpair, console) remain sequential —
// they bypass IoRing entirely and use their own wait mechanisms.
//
// Returns bytes transferred on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#include "scatter_io_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/struct_iovec.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/libc_assert.h"
#include "src/__support/OSUtil/windows/io/batch_engine.h"
#include "src/__support/OSUtil/windows/io/console_file_io.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/OSUtil/windows/resource/rlimit_query.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Maximum number of iovec elements per scatter/gather call. Matches
// SCATTER_IOV_MAX from <limits.h> (defined in llvm-libc-macros/limits-macros.h).
// Defined locally to avoid pulling public headers into internal code.
constexpr int SCATTER_IOV_MAX = 1024;

// =========================================================================
// Console pre-wait
// =========================================================================

int console_pre_wait(OpenFileDescription *ofd, HANDLE h) {
  if (!ofd->is_console())
    return 0;
  for (;;) {
    LARGE_INTEGER *tp = nullptr;
    LARGE_INTEGER nb_timeout;
    if (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK) {
      nb_timeout.QuadPart = 0;
      tp = &nb_timeout;
    }
    NTSTATUS ws = NtWaitForSingleObject(h, /*Alertable=*/TRUE, tp);
    if (ws == STATUS_TIMEOUT)
      return -EAGAIN;
    if (ws == STATUS_USER_APC || ws == STATUS_ALERTED) {
      auto *tss = signal_state::get_thread_state_noinit();
      if (tss)
        tss->handler_ran = false;
      if (signal_state::should_restart_syscall())
        continue;
      if (tss && !tss->handler_ran)
        continue;
      return -EINTR;
    }
    switch (console_tty::consume_pending_resize_events(h)) {
    case console_tty::PendingInputState::ReadyForRead:
      return 0;
    case console_tty::PendingInputState::RetryWait:
    case console_tty::PendingInputState::WouldBlock:
      if (tp)
        return -EAGAIN;
      continue;
    }
    return 0;
  }
}

// =========================================================================
// SIGPIPE helper
// =========================================================================

void raise_sigpipe_if_needed() {
  signal_state::generate_standard_signal_for_current_thread(SIGPIPE);
}

// Clamp a buffer length to the ULONG (32-bit) range accepted by IoRing SQEs.
// If clamping occurs (len > 4GB), callers in batch paths must not process
// further iovec elements — subsequent elements would be assigned wrong file
// offsets, breaking contiguous data semantics.
ULONG clamp_iov_len(size_t len) {
  return (len > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<ULONG>(len);
}

// =========================================================================
// Sequential single-element IO Ring helpers
// =========================================================================
//
// Used as fallback when batching isn't possible (O_APPEND writes, mixed
// transport fds, overflow beyond SQ capacity).

ssize_t ioring_read_one(OpenFileDescription *ofd, ioring::ThreadRing *tr,
                        HANDLE h, void *buf, size_t count) {
  ioring::RingState *ring = &tr->ring;
  ULONG io_len = clamp_iov_len(count);

  // Pre-claim position range for seekable fds to avoid overlapping reads
  // when concurrent threads share the same OFD (via dup'd fds).
  bool seekable = ofd->is_seekable();
  int64_t pos = 0;
  if (seekable) {
    pos = ofd->disk().position.fetch_add(static_cast<int64_t>(io_len),
                                  cpp::MemoryOrder::ACQ_REL);
  }

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  auto *sqe = ioring::push_read(ring, h, buf, io_len,
                                static_cast<ULONGLONG>(pos),
                                tag.as_user_data());
  if (!sqe) {
    if (seekable)
      ofd->disk().position.fetch_sub(static_cast<int64_t>(io_len),
                              cpp::MemoryOrder::ACQ_REL);
    return -EIO;
  }

  // Pipes use overlapped handles with nonblock-aware completion.
  // Other fd types use synchronous submit-and-wait.
  FileIOResult result{0, 0};
  if (ofd->is_pipe()) {
    int pflags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    result = ioring_pipe_io(tr, h, gen, pflags & O_NONBLOCK);
  } else {
    result = ioring_submit_and_wait(tr, h, gen);
  }

  if (result.has_error()) {
    if (seekable)
      ofd->disk().position.fetch_sub(static_cast<int64_t>(io_len),
                              cpp::MemoryOrder::ACQ_REL);
    // STATUS_PIPE_BROKEN on read = all writers closed = EOF per POSIX.
    if (result.error == EPIPE)
      return 0;
    return -result.error;
  }

  // Give back unused pre-claimed bytes on short read or EOF.
  if (seekable && result.value < static_cast<size_t>(io_len)) {
    int64_t unused = static_cast<int64_t>(io_len) -
                     static_cast<int64_t>(result.value);
    ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }

  return static_cast<ssize_t>(result.value);
}

ssize_t ioring_write_one(OpenFileDescription *ofd, ioring::ThreadRing *tr,
                         HANDLE h, const void *buf, size_t count) {
  ioring::RingState *ring = &tr->ring;
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  ULONG io_len = clamp_iov_len(count);
  bool seekable = ofd->is_seekable();
  bool is_append = seekable && (flags & O_APPEND);

  // Pre-claim position range for non-append seekable writes.
  // O_APPEND uses FILE_WRITE_TO_END_OF_FILE64 — kernel picks offset.
  ULONGLONG offset = 0;
  bool claiming = false;
  if (seekable) {
    if (is_append) {
      offset = FILE_WRITE_TO_END_OF_FILE64;
    } else {
      int64_t slen = static_cast<int64_t>(io_len);
      int64_t cur = ofd->disk().position.fetch_add(slen, cpp::MemoryOrder::ACQ_REL);
      // RLIMIT_FSIZE: check if this write would exceed the limit.
      rlim_t fsize = windows::get_fsize_limit();
      if (fsize != RLIM_INFINITY &&
          cur + slen > static_cast<int64_t>(fsize)) {
        ofd->disk().position.fetch_sub(slen, cpp::MemoryOrder::ACQ_REL);
        signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
        return -EFBIG;
      }
      offset = static_cast<ULONGLONG>(cur);
      claiming = true;
    }
  }

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  windows::prefault_read_pages(buf, io_len);
  auto *sqe = ioring::push_write(ring, h, buf, io_len,
                                 offset, tag.as_user_data());
  if (!sqe) {
    if (claiming)
      ofd->disk().position.fetch_sub(static_cast<int64_t>(io_len),
                              cpp::MemoryOrder::ACQ_REL);
    return -EIO;
  }

  // Pipes use overlapped handles with nonblock-aware completion.
  // Other fd types use synchronous submit-and-wait.
  FileIOResult result{0, 0};
  if (ofd->is_pipe()) {
    result = ioring_pipe_io(tr, h, gen, flags & O_NONBLOCK);
  } else {
    result = ioring_submit_and_wait(tr, h, gen);
  }

  if (result.has_error()) {
    if (claiming)
      ofd->disk().position.fetch_sub(static_cast<int64_t>(io_len),
                              cpp::MemoryOrder::ACQ_REL);
    return -result.error;
  }

  if (claiming && result.value < static_cast<size_t>(io_len)) {
    int64_t unused = static_cast<int64_t>(io_len) -
                     static_cast<int64_t>(result.value);
    ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }

  // O_APPEND: query new EOF and update cached position for lseek(SEEK_CUR).
  if (is_append) {
    IO_STATUS_BLOCK iosb = {};
    FILE_STANDARD_INFORMATION fsi;
    NTSTATUS qs = ::NtQueryInformationFile(h, &iosb, &fsi, sizeof(fsi),
                                           FileStandardInformation);
    if (NT_SUCCESS(qs))
      ofd->disk().position.store(fsi.EndOfFile.QuadPart,
                          cpp::MemoryOrder::RELEASE);
  }

  return static_cast<ssize_t>(result.value);
}

// =========================================================================
// Transport dispatch — single-element (for non-IoRing transports)
// =========================================================================

ssize_t dispatch_read_one(OpenFileDescription *ofd, ioring::ThreadRing *tr,
                          HANDLE h, void *buf, size_t count) {
  if (ofd->is_fifo()) {
    int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    return fifo_read(ofd->fifo_channel(), buf, count, flags);
  }

  if (ofd->kind == FileKind::AfdSocket) {
    auto *state = ofd->afd_socket();
    long connect_state = socket_reap_connect_if_needed(state);
    if (connect_state < 0)
      return connect_state;
    if (state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE) & 1)
      return 0;
  }

  if (ofd->is_console())
    return console_tty::read(ofd, buf, count);
  if (vt_pty::is_pty(ofd))
    return vt_pty::read(ofd, buf, count);

  return ioring_read_one(ofd, tr, h, buf, count);
}

ssize_t dispatch_write_one(OpenFileDescription *ofd, ioring::ThreadRing *tr,
                           HANDLE h, const void *buf, size_t count) {
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);

  if (ofd->kind == FileKind::SocketPair) {
    auto *sp = ofd->socket_pair();
    uint8_t shut = sp->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (shut & SP_SHUT_WR) {
      raise_sigpipe_if_needed();
      return -EPIPE;
    }
    ssize_t r = fifo_write(sp->write_ch, buf, count, flags);
    if (r == -EPIPE)
      raise_sigpipe_if_needed();
    return r;
  }

  if (ofd->is_fifo()) {
    ssize_t r = fifo_write(ofd->fifo_channel(), buf, count, flags);
    if (r == -EPIPE)
      raise_sigpipe_if_needed();
    return r;
  }

  if (ofd->kind == FileKind::AfdSocket) {
    auto *state = ofd->afd_socket();
    long connect_state = socket_reap_connect_if_needed(state);
    if (connect_state < 0)
      return connect_state;
    if (state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE) & 2) {
      raise_sigpipe_if_needed();
      return -EPIPE;
    }
  }

  if (ofd->is_console())
    return console_tty::write(ofd, buf, count);
  if (vt_pty::is_pty(ofd))
    return vt_pty::write(ofd, buf, count);

  return ioring_write_one(ofd, tr, h, buf, count);
}

// =========================================================================
// Batched readv via BatchEngine
// =========================================================================
//
// Pre-claims the full byte range from the file position, pushes all iovec
// elements as SQEs with pre-computed offsets, submits in one syscall, then
// drains CQEs. Adjusts position by the actual bytes transferred.
//
// Pre-computed offsets are correct for the common case (full reads). On
// short reads at EOF: elements past EOF return STATUS_END_OF_FILE with
// 0 bytes (buffer untouched). The successful prefix logic in BatchEngine
// discards results after the first short read.

ssize_t readv_batched(OpenFileDescription *ofd, ioring::ThreadRing *tr,
                      HANDLE h, const struct iovec *iov, int iovcnt) {
  // AFD socket pre-checks (connect, shutdown).
  if (ofd->kind == FileKind::AfdSocket) {
    auto *state = ofd->afd_socket();
    long cs = socket_reap_connect_if_needed(state);
    if (cs < 0)
      return cs;
    if (state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE) & 1)
      return 0; // SHUT_RD → EOF
  }

  // Console: pre-wait, then fall through to batched reads.
  int wait_ret = console_pre_wait(ofd, h);
  if (wait_ret < 0)
    return wait_ret;

  // Compute total iov length for position pre-claim, using clamped values.
  // Stops at the first element that gets clamped (>4GB) — subsequent elements
  // won't be pushed to IoRing, so we must not pre-claim their range.
  // This eliminates the phantom position window where concurrent threads
  // could see a falsely-advanced position.
  size_t total_len = 0;
  int non_empty = 0;
  int clamp_stop_idx = iovcnt; // Index beyond which we stop processing.
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > 0) {
      ULONG clamped = clamp_iov_len(iov[i].iov_len);
      if (static_cast<size_t>(clamped) > SIZE_MAX - total_len)
        return -EINVAL;
      total_len += clamped;
      ++non_empty;
      if (clamped < iov[i].iov_len) {
        clamp_stop_idx = i + 1; // Process up to and including this element.
        break;
      }
    }
  }
  if (non_empty == 0)
    return 0;

  // Validate total_len fits in int64_t before position pre-claim.
  if (total_len > static_cast<size_t>(INT64_MAX))
    return -EINVAL;
  int64_t stotal_len = static_cast<int64_t>(total_len);

  // Pre-claim position range (seekable fds only).
  bool seekable = ofd->is_seekable();
  int64_t base_pos = 0;
  if (seekable) {
    base_pos = ofd->disk().position.fetch_add(stotal_len,
                                       cpp::MemoryOrder::ACQ_REL);
    // Validate the claimed range doesn't overflow.
    if (base_pos < 0 ||
        static_cast<uint64_t>(base_pos) >
            static_cast<uint64_t>(INT64_MAX) - total_len) {
      ofd->disk().position.fetch_sub(stotal_len, cpp::MemoryOrder::ACQ_REL);
      return -EINVAL;
    }
  }

  // Push iovec elements as SQEs. Chunk into rounds of SQ capacity.
  // Use registered buffers for O_DIRECT files — MDL pre-pinning eliminates
  // per-op page table walks (67% faster for unbuffered I/O, Research D4).
  bool use_regbuf =
      ofd->status_flags.load(cpp::MemoryOrder::RELAXED) & O_DIRECT;
  ioring::BatchEngine batch;
  batch.init(tr, h, use_regbuf);

  ULONGLONG offset = static_cast<ULONGLONG>(base_pos);
  int iov_idx = 0;

  // Accumulate total bytes across potentially multiple batch rounds.
  ssize_t grand_total = 0;

  while (iov_idx < clamp_stop_idx) {
    // Fill batch with as many elements as the ring can hold.
    while (iov_idx < clamp_stop_idx &&
           batch.count() < ioring::BATCH_MAX_OPS) {
      if (iov[iov_idx].iov_len == 0) {
        ++iov_idx;
        continue;
      }
      ULONG req_len = clamp_iov_len(iov[iov_idx].iov_len);
      int slot = batch.push_read(iov[iov_idx].iov_base, req_len, offset);
      if (slot < 0)
        break; // SQ full — submit what we have.
      offset += req_len;
      ++iov_idx;
    }

    if (batch.count() == 0)
      break;

    // Submit + drain via pipeline.
    ioring::BatchResult br = batch.submit_and_drain();

    grand_total += static_cast<ssize_t>(br.total_bytes);

    if (br.error) {
      if (grand_total > 0)
        break; // POSIX: return bytes already transferred, not error.
      // STATUS_PIPE_BROKEN on read = all writers closed = EOF per POSIX.
      if (br.error == EPIPE) {
        if (seekable)
          ofd->disk().position.fetch_sub(stotal_len, cpp::MemoryOrder::ACQ_REL);
        return 0;
      }
      // Adjust position for the bytes we pre-claimed but didn't use.
      if (seekable)
        ofd->disk().position.fetch_sub(stotal_len, cpp::MemoryOrder::ACQ_REL);
      return -br.error;
    }

    // Short read or EOF in this batch — stop.
    if (br.completed < batch.count())
      break;

    // Prepare next round if more iovecs remain within clamped range.
    if (iov_idx < clamp_stop_idx)
      batch.init(tr, h, use_regbuf);
  }

  // Adjust position: give back bytes we pre-claimed but didn't transfer.
  if (seekable) {
    int64_t unused = stotal_len - static_cast<int64_t>(grand_total);
    if (unused > 0)
      ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }

  return grand_total;
}

// =========================================================================
// Batched writev via BatchEngine
// =========================================================================
//
// Same pattern as readv_batched, except:
//   - O_APPEND: falls back to sequential (kernel picks offset per SQE,
//     batching would produce non-contiguous writes)
//   - SIGPIPE on EPIPE results
//   - Post-append position query for O_APPEND seekable fds

ssize_t writev_batched(OpenFileDescription *ofd, ioring::ThreadRing *tr,
                       HANDLE h, const struct iovec *iov, int iovcnt) {
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);

  // AFD socket pre-checks.
  if (ofd->kind == FileKind::AfdSocket) {
    auto *state = ofd->afd_socket();
    long cs = socket_reap_connect_if_needed(state);
    if (cs < 0)
      return cs;
    if (state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE) & 2) {
      raise_sigpipe_if_needed();
      return -EPIPE;
    }
  }

  // O_APPEND: can't batch (kernel picks offset per SQE — batched SQEs
  // would produce non-contiguous writes if another writer interposes).
  // Fall back to sequential one-at-a-time.
  if (flags & O_APPEND) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
      if (iov[i].iov_len == 0)
        continue;
      ssize_t n = ioring_write_one(ofd, tr, h, iov[i].iov_base,
                                   iov[i].iov_len);
      if (n < 0) {
        if (total > 0)
          return total;
        if (n == -EPIPE)
          raise_sigpipe_if_needed();
        return n;
      }
      total += n;
      if (static_cast<size_t>(n) < iov[i].iov_len)
        break;
    }
    return total;
  }

  // Compute total length using clamped values, stopping at first clamp.
  size_t total_len = 0;
  int non_empty = 0;
  int clamp_stop_idx = iovcnt;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > 0) {
      ULONG clamped = clamp_iov_len(iov[i].iov_len);
      if (static_cast<size_t>(clamped) > SIZE_MAX - total_len)
        return -EINVAL;
      total_len += clamped;
      ++non_empty;
      if (clamped < iov[i].iov_len) {
        clamp_stop_idx = i + 1;
        break;
      }
    }
  }
  if (non_empty == 0)
    return 0;

  // Validate total_len fits in int64_t before position pre-claim.
  if (total_len > static_cast<size_t>(INT64_MAX))
    return -EINVAL;
  int64_t stotal_len = static_cast<int64_t>(total_len);

  bool seekable = ofd->is_seekable();
  int64_t base_pos = 0;
  if (seekable) {
    base_pos = ofd->disk().position.fetch_add(stotal_len,
                                       cpp::MemoryOrder::ACQ_REL);
    if (base_pos < 0 ||
        static_cast<uint64_t>(base_pos) >
            static_cast<uint64_t>(INT64_MAX) - total_len) {
      ofd->disk().position.fetch_sub(stotal_len, cpp::MemoryOrder::ACQ_REL);
      return -EINVAL;
    }
    // RLIMIT_FSIZE: check if the batched write would exceed the limit.
    rlim_t fsize = windows::get_fsize_limit();
    if (fsize != RLIM_INFINITY &&
        base_pos + stotal_len > static_cast<int64_t>(fsize)) {
      ofd->disk().position.fetch_sub(stotal_len, cpp::MemoryOrder::ACQ_REL);
      signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
      return -EFBIG;
    }
  }

  bool use_regbuf = flags & O_DIRECT;
  ioring::BatchEngine batch;
  batch.init(tr, h, use_regbuf);

  ULONGLONG offset = seekable ? static_cast<ULONGLONG>(base_pos) : 0;
  int iov_idx = 0;
  ssize_t grand_total = 0;

  while (iov_idx < clamp_stop_idx) {
    while (iov_idx < clamp_stop_idx &&
           batch.count() < ioring::BATCH_MAX_OPS) {
      if (iov[iov_idx].iov_len == 0) {
        ++iov_idx;
        continue;
      }
      ULONG req_len = clamp_iov_len(iov[iov_idx].iov_len);
      int slot = batch.push_write(iov[iov_idx].iov_base, req_len, offset);
      if (slot < 0)
        break;
      if (seekable)
        offset += req_len;
      ++iov_idx;
    }

    if (batch.count() == 0)
      break;

    ioring::BatchResult br = batch.submit_and_drain();

    grand_total += static_cast<ssize_t>(br.total_bytes);

    if (br.error) {
      if (grand_total > 0) {
        if (br.error == EPIPE)
          raise_sigpipe_if_needed();
        break;
      }
      if (seekable)
        ofd->disk().position.fetch_sub(stotal_len, cpp::MemoryOrder::ACQ_REL);
      if (br.error == EPIPE)
        raise_sigpipe_if_needed();
      return -br.error;
    }

    if (br.completed < batch.count())
      break;

    if (iov_idx < clamp_stop_idx)
      batch.init(tr, h, use_regbuf);
  }

  // Adjust position for short writes.
  if (seekable) {
    int64_t unused = stotal_len - static_cast<int64_t>(grand_total);
    if (unused > 0)
      ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }

  return grand_total;
}

// =========================================================================
// Check if an fd is eligible for batched IoRing I/O
// =========================================================================

bool is_ioring_batchable_read(OpenFileDescription *ofd) {
  // FIFO and socketpair use their own ring buffer transport — not IoRing.
  if (ofd->is_fifo() || ofd->kind == FileKind::SocketPair)
    return false;
  // NT pipes use overlapped handles with async completion. BatchEngine's
  // submit(ring, N) blocks until ALL N CQEs arrive, which deadlocks when
  // only partial data is available. Route pipes through sequential dispatch
  // where each element gets its own submit + nonblock-aware wait.
  if (ofd->is_pipe())
    return false;
  // Console byte streams use direct NtReadFile after an alertable readiness
  // wait, matching the intended ReadFile-style data path.
  if (ofd->is_console())
    return false;
  if (vt_pty::is_pty(ofd))
    return false;
  // Everything else — disk files and AFD sockets — goes through IoRing.
  return true;
}

bool is_ioring_batchable_write(OpenFileDescription *ofd) {
  // FIFO write end.
  if (ofd->is_fifo())
    return false;
  // Socketpair write channel.
  if (ofd->kind == FileKind::SocketPair)
    return false;
  // NT pipes: same as read — sequential dispatch avoids batch deadlock.
  if (ofd->is_pipe())
    return false;
  if (ofd->is_console())
    return false;
  if (vt_pty::is_pty(ofd))
    return false;
  return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// readv engine
// ---------------------------------------------------------------------------

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  if (iovcnt <= 0 || iovcnt > SCATTER_IOV_MAX)
    return -EINVAL;
  if (!iov)
    return -EFAULT;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;
  if (ofd->is_virtual_dir())
    return -EISDIR;

  // /dev/zero emulation: zero-fill all iovec buffers.
  if (ofd->is_dev_zero()) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
      if (iov[i].iov_len == 0)
        continue;
      __builtin_memset(iov[i].iov_base, 0, iov[i].iov_len);
      total += static_cast<ssize_t>(iov[i].iov_len);
    }
    return total;
  }

  HANDLE h = ofd->handle;

  // Batched path: disk files, AFD sockets, consoles — IoRing-eligible.
  if (is_ioring_batchable_read(ofd)) {
    auto *tr = ioring::get_thread_ring();
    if (!tr)
      return -EIO;
    return readv_batched(ofd, tr, h, iov, iovcnt);
  }

  // Sequential path: FIFO ring buffers, socketpair, and NT pipes.
  // FIFO/socketpair bypass IoRing; pipes use IoRing per-element with
  // nonblock-aware completion (overlapped handles).
  ioring::ThreadRing *tr = nullptr;
  if (ofd->is_pipe()) {
    tr = ioring::get_thread_ring();
    if (!tr)
      return -EIO;
  }

  ssize_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len == 0)
      continue;
    ssize_t n =
        dispatch_read_one(ofd, tr, h, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) {
      if (total > 0)
        return total;
      return n;
    }
    if (n == 0)
      break;
    total += n;
    if (static_cast<size_t>(n) < iov[i].iov_len)
      break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// writev engine
// ---------------------------------------------------------------------------

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  if (iovcnt <= 0 || iovcnt > SCATTER_IOV_MAX)
    return -EINVAL;
  if (!iov)
    return -EFAULT;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;
  if (ofd->is_virtual_dir())
    return -EISDIR;

  // /dev/zero emulation: discard writes, return total length.
  if (ofd->is_dev_zero()) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i)
      total += static_cast<ssize_t>(iov[i].iov_len);
    return total;
  }

  HANDLE h = ofd->handle;

  // Batched path: IoRing-eligible fds.
  if (is_ioring_batchable_write(ofd)) {
    auto *tr = ioring::get_thread_ring();
    if (!tr)
      return -EIO;
    return writev_batched(ofd, tr, h, iov, iovcnt);
  }

  // Sequential path: FIFO, socketpair, and NT pipes.
  // Pipes use IoRing per-element with nonblock-aware completion.
  ioring::ThreadRing *tr = nullptr;
  if (ofd->is_pipe()) {
    tr = ioring::get_thread_ring();
    if (!tr)
      return -EIO;
  }

  ssize_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len == 0)
      continue;
    ssize_t n = dispatch_write_one(ofd, tr, h, iov[i].iov_base,
                                   iov[i].iov_len);
    if (n < 0) {
      if (total > 0)
        return total;
      return n;
    }
    total += n;
    if (static_cast<size_t>(n) < iov[i].iov_len)
      break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// preadv engine
// ---------------------------------------------------------------------------

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  if (iovcnt <= 0 || iovcnt > SCATTER_IOV_MAX)
    return -EINVAL;
  if (!iov)
    return -EFAULT;
  if (offset < 0)
    return -EINVAL;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  // /dev/zero emulation: zero-fill all iovec buffers.
  if (ofd->is_dev_zero()) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
      if (iov[i].iov_len == 0)
        continue;
      __builtin_memset(iov[i].iov_base, 0, iov[i].iov_len);
      total += static_cast<ssize_t>(iov[i].iov_len);
    }
    return total;
  }

  if (!ofd->is_seekable())
    return -ESPIPE;

  HANDLE h = ofd->handle;

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;

  // Compute total iov length for overflow validation.
  size_t total_len = 0;
  int non_empty = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > 0) {
      if (iov[i].iov_len > SIZE_MAX - total_len)
        return -EINVAL;
      total_len += iov[i].iov_len;
      ++non_empty;
    }
  }
  if (non_empty == 0)
    return 0;

  // Reject if offset + total length would overflow the signed 64-bit file
  // offset that the kernel uses. The kernel would reject individual SQEs
  // past INT64_MAX, but checking upfront avoids silent ULONGLONG wraparound
  // in the offset accumulator below.
  if (total_len > static_cast<size_t>(INT64_MAX) ||
      static_cast<uint64_t>(offset) >
          static_cast<uint64_t>(INT64_MAX) - total_len)
    return -EINVAL;

  bool use_regbuf =
      ofd->status_flags.load(cpp::MemoryOrder::RELAXED) & O_DIRECT;
  ioring::BatchEngine batch;
  batch.init(tr, h, use_regbuf);

  ULONGLONG file_offset = static_cast<ULONGLONG>(offset);
  int iov_idx = 0;
  ssize_t grand_total = 0;

  while (iov_idx < iovcnt) {
    while (iov_idx < iovcnt && batch.count() < ioring::BATCH_MAX_OPS) {
      if (iov[iov_idx].iov_len == 0) {
        ++iov_idx;
        continue;
      }
      size_t elem_len = iov[iov_idx].iov_len;
      ULONG req_len = clamp_iov_len(elem_len);
      int slot = batch.push_read(iov[iov_idx].iov_base, req_len, file_offset);
      if (slot < 0)
        break;
      file_offset += req_len;
      ++iov_idx;
      if (req_len < elem_len) {
        iov_idx = iovcnt; // Clamped — stop to preserve data contiguity.
        break;
      }
    }

    if (batch.count() == 0)
      break;

    ioring::BatchResult br = batch.submit_and_drain();

    grand_total += static_cast<ssize_t>(br.total_bytes);

    if (br.error) {
      if (grand_total > 0)
        break;
      return -br.error;
    }

    if (br.completed < batch.count())
      break;

    if (iov_idx < iovcnt)
      batch.init(tr, h, use_regbuf);
  }

  // preadv does NOT advance position.
  return grand_total;
}

// ---------------------------------------------------------------------------
// pwritev engine
// ---------------------------------------------------------------------------

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  if (iovcnt <= 0 || iovcnt > SCATTER_IOV_MAX)
    return -EINVAL;
  if (!iov)
    return -EFAULT;
  if (offset < 0)
    return -EINVAL;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  // /dev/zero emulation: discard writes, return total length.
  if (ofd->is_dev_zero()) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i)
      total += static_cast<ssize_t>(iov[i].iov_len);
    return total;
  }

  if (!ofd->is_seekable())
    return -ESPIPE;

  HANDLE h = ofd->handle;

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;

  size_t total_len = 0;
  int non_empty = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > 0) {
      if (iov[i].iov_len > SIZE_MAX - total_len)
        return -EINVAL;
      total_len += iov[i].iov_len;
      ++non_empty;
    }
  }
  if (non_empty == 0)
    return 0;

  // Reject if offset + total length would overflow signed 64-bit file offset.
  if (total_len > static_cast<size_t>(INT64_MAX) ||
      static_cast<uint64_t>(offset) >
          static_cast<uint64_t>(INT64_MAX) - total_len)
    return -EINVAL;

  // RLIMIT_FSIZE: check if the positioned scatter write would exceed the limit.
  rlim_t fsize = windows::get_fsize_limit();
  if (fsize != RLIM_INFINITY &&
      static_cast<int64_t>(offset) + static_cast<int64_t>(total_len) >
          static_cast<int64_t>(fsize)) {
    signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
    return -EFBIG;
  }

  bool use_regbuf =
      ofd->status_flags.load(cpp::MemoryOrder::RELAXED) & O_DIRECT;
  ioring::BatchEngine batch;
  batch.init(tr, h, use_regbuf);

  ULONGLONG file_offset = static_cast<ULONGLONG>(offset);
  int iov_idx = 0;
  ssize_t grand_total = 0;

  while (iov_idx < iovcnt) {
    while (iov_idx < iovcnt && batch.count() < ioring::BATCH_MAX_OPS) {
      if (iov[iov_idx].iov_len == 0) {
        ++iov_idx;
        continue;
      }
      size_t elem_len = iov[iov_idx].iov_len;
      ULONG req_len = clamp_iov_len(elem_len);
      int slot = batch.push_write(iov[iov_idx].iov_base, req_len, file_offset);
      if (slot < 0)
        break;
      file_offset += req_len;
      ++iov_idx;
      if (req_len < elem_len) {
        iov_idx = iovcnt; // Clamped — stop to preserve data contiguity.
        break;
      }
    }

    if (batch.count() == 0)
      break;

    ioring::BatchResult br = batch.submit_and_drain();

    grand_total += static_cast<ssize_t>(br.total_bytes);

    // pwritev targets seekable fds only (non-seekable rejected with ESPIPE
    // at entry), so EPIPE/SIGPIPE cannot occur — no pipe signaling needed.
    if (br.error) {
      if (grand_total > 0)
        break;
      return -br.error;
    }

    if (br.completed < batch.count())
      break;

    if (iov_idx < iovcnt)
      batch.init(tr, h, use_regbuf);
  }

  // pwritev does NOT advance position.
  return grand_total;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
