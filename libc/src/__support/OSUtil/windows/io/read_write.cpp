//===-- Windows kernel functions for read/write ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Data transfer kernel functions for Windows. These implement Linux syscall
// semantics in userspace: bytes transferred on success, -errno on failure.
// Called from the syscall_impl dispatch (syscall.h) after constant folding.
//
// Responsibilities that live here (the "kernel" layer):
//   - fd_table lookup and OFD access
//   - Transport dispatch (FIFO ring buffer, socketpair, console, IO Ring)
//   - Atomic file position claiming/unclaiming
//   - SA_RESTART retry (Linux kernel does this in restart_block)
//   - SIGPIPE generation on broken pipe (Linux kernel does this in pipe_write)
//   - Cancellation checks between SA_RESTART retries (kernel checks fatal
//     signals between restart iterations)
//
// NOT here (libc entry point concerns):
//   - cancel_check() at POSIX cancellation point entry
//   - errno/return-value conversion (syscall_return handles this)
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/File/file_io_result.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/io/batch_engine.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/console_file_io.h"
#include "src/__support/OSUtil/windows/io.h"
#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/ipc/inotify_ops.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/resource/rlimit_query.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/pthread/cancel_internal.h"
#include "src/__support/OSUtil/windows/signal/signal.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Pipeline chunk size for large O_DIRECT reads/writes. See file_ops.cpp.
static constexpr ULONG PIPELINE_CHUNK_SIZE = 65536;

// ---------------------------------------------------------------------------
// ioring_simple_read / ioring_simple_write — shared IoRing single-SQE path
// ---------------------------------------------------------------------------
// Used by char, afd_socket, and other non-seekable kinds that do a
// straightforward submit-and-wait at offset 0 with no position tracking.

static ssize_t ioring_simple_read(HANDLE h, void *buf, size_t count) {
  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;
  ULONG io_len = (count > 0xFFFFFFFFULL) ? 0xFFFFFFFFU
                                         : static_cast<ULONG>(count);
  uint32_t gen = tr->next_generation();
  FileIOResult result = read_single_op_wait(&tr->ring, tr->event,
                                            tr->wait_event, h, buf,
                                            io_len, 0, gen);
  if (result.has_error())
    return -result.error;
  return static_cast<ssize_t>(result.value);
}

static ssize_t ioring_simple_write(HANDLE h, const void *buf, size_t count) {
  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;
  ULONG io_len = (count > 0xFFFFFFFFULL) ? 0xFFFFFFFFU
                                         : static_cast<ULONG>(count);
  uint32_t gen = tr->next_generation();
  FileIOResult result = write_single_op_wait(&tr->ring, tr->event,
                                             tr->wait_event, h, buf,
                                             io_len, 0, gen);
  if (result.has_error())
    return -result.error;
  return static_cast<ssize_t>(result.value);
}

// =========================================================================
// internal::read — kernel function
// =========================================================================
// Dispatch via the per-kind ops table: single indirect call, O(1).

ssize_t read(int fd, void *buf, size_t count) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;
  return ofd->ops->read(ofd, buf, count);
}

static void raise_sigpipe_if_needed() {
  signal_state::generate_standard_signal_for_current_thread(SIGPIPE);
}

// RLIMIT_FSIZE enforcement: check whether writing `count` bytes starting
// at `file_offset` would exceed the soft file size limit. For O_APPEND
// writes where the offset isn't known before the write, `file_offset`
// should be the current file size (queried beforehand).
// Returns true if the write is allowed, false if it exceeds the limit
// (SIGXFSZ raised and caller should return -EFBIG).
static bool check_fsize_limit(int64_t file_offset, size_t count) {
  rlim_t fsize = windows::get_fsize_limit();
  if (fsize == RLIM_INFINITY)
    return true;
  int64_t end = file_offset + static_cast<int64_t>(count);
  if (end <= static_cast<int64_t>(fsize))
    return true;
  signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
  return false;
}

// =========================================================================
// internal::write — kernel function
// =========================================================================
// Dispatch via the per-kind ops table: single indirect call, O(1).

ssize_t write(int fd, const void *buf, size_t count) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;
  return ofd->ops->write(ofd, buf, count);
}

// =========================================================================
// Per-kind FileOps read/write/release implementations
// =========================================================================
// Each function takes an OFD (already validated: non-null, not O_PATH) and
// performs kind-specific I/O. Dispatched via ofd->ops->read/write from the
// read()/write() entry points above.

// --- IoRing helpers (shared by disk, pipe, char, afd_socket) ---


// --- Disk read/write ---

ssize_t disk_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  if (LIBC_UNLIKELY(count == 0))
    return 0;

  HANDLE h = ofd->handle;
  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;
  ioring::RingState *ring = &tr->ring;

  if (LIBC_UNLIKELY(count > static_cast<size_t>(INT64_MAX)))
    return -EOVERFLOW;
  int64_t scount = static_cast<int64_t>(count);
  int64_t pos = ofd->disk().position.fetch_add(scount,
                                                cpp::MemoryOrder::ACQ_REL);
  if (LIBC_UNLIKELY(pos < 0 ||
      static_cast<uint64_t>(pos) > static_cast<uint64_t>(INT64_MAX) - count)) {
    ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
    return -EOVERFLOW;
  }

  ULONG io_len = static_cast<ULONG>(count <= 0xFFFFFFFFULL ? count
                                                            : 0xFFFFFFFFU);

  // Large O_DIRECT reads: pipeline chunks for NVMe QD.
  int sflags = ofd->status_flags.load(cpp::MemoryOrder::RELAXED);
  if (LIBC_UNLIKELY((sflags & O_DIRECT) && io_len > PIPELINE_CHUNK_SIZE)) {
    ioring::BatchEngine batch;
    batch.init(tr, h, /*use_regbuf=*/true);

    char *p = static_cast<char *>(buf);
    ULONGLONG off = static_cast<ULONGLONG>(pos);
    ULONG remaining = io_len;
    while (remaining > 0 && batch.count() < ioring::BATCH_MAX_OPS) {
      ULONG chunk = remaining < PIPELINE_CHUNK_SIZE ? remaining
                                                    : PIPELINE_CHUNK_SIZE;
      if (batch.push_read(p, chunk, off) < 0)
        break;
      p += chunk;
      off += chunk;
      remaining -= chunk;
    }

    ioring::BatchResult br = batch.submit_and_drain();
    if (br.error) {
      ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
      return -br.error;
    }
    if (br.total_bytes < count) {
      int64_t unused = scount - static_cast<int64_t>(br.total_bytes);
      ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
    }
    return static_cast<ssize_t>(br.total_bytes);
  }

  // Single-SQE path: combined push+submit+pop for cached I/O.
  uint32_t gen = tr->next_generation();
  FileIOResult result = read_single_op_wait(ring, tr->event, tr->wait_event,
                                            h, buf, io_len,
                                            static_cast<ULONGLONG>(pos), gen);
  if (result.has_error()) {
    ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
    if (result.error == EPIPE)
      return 0; // EOF
    return -result.error;
  }

  if (result.value < count) {
    int64_t unused = scount - static_cast<int64_t>(result.value);
    ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }
  return static_cast<ssize_t>(result.value);
}

ssize_t disk_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count) {
  if (LIBC_UNLIKELY(count == 0))
    return 0;

  HANDLE h = ofd->handle;
  int flags = ofd->status_flags.load(cpp::MemoryOrder::RELAXED);

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;
  ioring::RingState *ring = &tr->ring;

  ULONGLONG offset = 0;
  bool claiming_position = false;
  if (LIBC_UNLIKELY((flags & O_APPEND) != 0)) {
    offset = FILE_WRITE_TO_END_OF_FILE64;
    rlim_t fsize = windows::get_fsize_limit();
    if (LIBC_UNLIKELY(fsize != RLIM_INFINITY)) {
      IO_STATUS_BLOCK fsize_iosb = {};
      FILE_STANDARD_INFORMATION fsi;
      NTSTATUS qs = ::NtQueryInformationFile(h, &fsize_iosb, &fsi,
                                             sizeof(fsi),
                                             FileStandardInformation);
      if (NT_SUCCESS(qs) && !check_fsize_limit(fsi.EndOfFile.QuadPart, count))
        return -EFBIG;
    }
  } else {
    if (LIBC_UNLIKELY(count > static_cast<size_t>(INT64_MAX)))
      return -EOVERFLOW;
    int64_t scount = static_cast<int64_t>(count);
    int64_t cur = ofd->disk().position.fetch_add(scount,
                                                  cpp::MemoryOrder::ACQ_REL);
    if (LIBC_UNLIKELY(cur < 0 ||
        static_cast<uint64_t>(cur) >
            static_cast<uint64_t>(INT64_MAX) - count)) {
      ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
      return -EOVERFLOW;
    }
    if (LIBC_UNLIKELY(!check_fsize_limit(cur, count))) {
      ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
      return -EFBIG;
    }
    offset = static_cast<ULONGLONG>(cur);
    claiming_position = true;
  }

  ULONG io_len = static_cast<ULONG>(count <= 0xFFFFFFFFULL ? count
                                                            : 0xFFFFFFFFU);

  // Large O_DIRECT writes on non-append: pipeline chunks for NVMe QD.
  // O_APPEND can't be pipelined (kernel picks offset per SQE).
  if (LIBC_UNLIKELY(claiming_position && (flags & O_DIRECT) &&
      io_len > PIPELINE_CHUNK_SIZE)) {
    ioring::BatchEngine batch;
    batch.init(tr, h, /*use_regbuf=*/true);

    const char *p = static_cast<const char *>(buf);
    ULONGLONG off = offset;
    ULONG remaining = io_len;
    while (remaining > 0 && batch.count() < ioring::BATCH_MAX_OPS) {
      ULONG chunk = remaining < PIPELINE_CHUNK_SIZE ? remaining
                                                    : PIPELINE_CHUNK_SIZE;
      if (batch.push_write(p, chunk, off) < 0)
        break;
      p += chunk;
      off += chunk;
      remaining -= chunk;
    }

    ioring::BatchResult br = batch.submit_and_drain();
    if (br.error) {
      ofd->disk().position.fetch_sub(static_cast<int64_t>(count),
                                     cpp::MemoryOrder::ACQ_REL);
      if (br.error == EPIPE)
        raise_sigpipe_if_needed();
      return -br.error;
    }
    if (br.total_bytes < count) {
      int64_t unused = static_cast<int64_t>(count) -
                       static_cast<int64_t>(br.total_bytes);
      ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
    }
    return static_cast<ssize_t>(br.total_bytes);
  }

  // Single-SQE path: prefault pages (volatile reads, ~1 cyc/page) so the
  // kernel can probe the user buffer directly — avoids the double-copy of
  // a registered bounce and the ~8K cycle NtQueryVirtualMemory of the old
  // materialize_file_private_range.
  // Single-SQE path: combined push+submit+pop for cached I/O.
  windows::prefault_read_pages(buf, io_len);
  uint32_t gen = tr->next_generation();
  FileIOResult result = write_single_op_wait(ring, tr->event, tr->wait_event,
                                             h, buf, io_len, offset, gen);

  if (result.has_error()) {
    if (claiming_position)
      ofd->disk().position.fetch_sub(static_cast<int64_t>(count),
                                     cpp::MemoryOrder::ACQ_REL);
    if (result.error == EPIPE)
      raise_sigpipe_if_needed();
    return -result.error;
  }

  if (claiming_position && result.value < count) {
    int64_t unused = static_cast<int64_t>(count) -
                     static_cast<int64_t>(result.value);
    ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }

  if ((flags & O_APPEND)) {
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

bool disk_release_aux(OpenFileDescription *ofd) {
  HANDLE sh = ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (sh)
    NtClose(sh);
  return true;
}

// --- Pipe read/write ---

ssize_t pipe_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  HANDLE h = ofd->handle;
  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;
  ioring::RingState *ring = &tr->ring;

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  ULONG io_len = (count > 0xFFFFFFFFULL) ? 0xFFFFFFFFU
                                         : static_cast<ULONG>(count);
  auto *sqe = ioring::push_read(ring, h, buf, io_len, 0,
                                tag.as_user_data());
  if (!sqe)
    return -EIO;

  int pflags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  FileIOResult result = ioring_pipe_io(ring, tr->event, tr->wait_event, h, gen,
                                       pflags & O_NONBLOCK);
  if (result.has_error()) {
    if (result.error == EPIPE)
      return 0; // EOF
    return -result.error;
  }

  // Clear readable_event if pipe drained.
  if (result.value > 0 && ofd->pipe_events()) {
    IO_STATUS_BLOCK pi_iosb = {};
    FILE_PIPE_LOCAL_INFORMATION pi = {};
    if (NT_SUCCESS(::NtQueryInformationFile(h, &pi_iosb, &pi, sizeof(pi),
                                            FilePipeLocalInformation)) &&
        pi.ReadDataAvailable == 0) {
      ::NtClearEvent(ofd->pipe_events()->readable_event);
    }
  }
  return static_cast<ssize_t>(result.value);
}

ssize_t pipe_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count) {
  HANDLE h = ofd->handle;
  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;
  ioring::RingState *ring = &tr->ring;

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  ULONG io_len = (count > 0xFFFFFFFFULL) ? 0xFFFFFFFFU
                                         : static_cast<ULONG>(count);
  auto *sqe = ioring::push_write(ring, h, buf, io_len, 0,
                                 tag.as_user_data());
  if (!sqe)
    return -EIO;

  int pflags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  FileIOResult result = ioring_pipe_io(ring, tr->event, tr->wait_event, h, gen,
                                       pflags & O_NONBLOCK);
  if (result.has_error()) {
    if (result.error == EPIPE)
      raise_sigpipe_if_needed();
    return -result.error;
  }

  // Signal readable_event for poll/epoll.
  if (result.value > 0 && ofd->pipe_events())
    ::NtSetEvent(ofd->pipe_events()->readable_event, nullptr);

  return static_cast<ssize_t>(result.value);
}

bool pipe_release_aux(OpenFileDescription *ofd) {
  FifoChannel *ev = ofd->pipe_events();
  if (ev)
    fifo_close_channel(ev);
  return true;
}

// --- Char read/write (generic character device, IoRing) ---

ssize_t char_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  return ioring_simple_read(ofd->handle, buf, count);
}

ssize_t char_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count) {
  return ioring_simple_write(ofd->handle, buf, count);
}

// --- AFD socket read/write ---

ssize_t afd_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  auto *state = ofd->afd_socket();
  long connect_state = socket_reap_connect_if_needed(state);
  if (connect_state < 0)
    return connect_state;
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 1)
    return 0; // SHUT_RD → EOF

  ssize_t r = ioring_simple_read(ofd->handle, buf, count);
  return (r == -EPIPE) ? 0 : r; // EPIPE on read = EOF
}

ssize_t afd_write_impl(OpenFileDescription *ofd, const void *buf,
                        size_t count) {
  auto *state = ofd->afd_socket();
  long connect_state = socket_reap_connect_if_needed(state);
  if (connect_state < 0)
    return connect_state;
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 2) {
    raise_sigpipe_if_needed();
    return -EPIPE;
  }

  ssize_t r = ioring_simple_write(ofd->handle, buf, count);
  if (r == -EPIPE)
    raise_sigpipe_if_needed();
  return r;
}

bool afd_release_aux(OpenFileDescription *ofd) {
  socket_state_free(ofd->afd_socket());
  return true;
}

// --- FIFO (mkfifo ring buffer) read/write ---

ssize_t fifo_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    ssize_t r = fifo_read(ofd->fifo_channel(), buf, count, flags);
    if (r >= 0)
      return r;
    cancel_check();
    if (r == -EINTR && signal_state::should_restart_syscall()) {
      cancel_check();
      continue;
    }
    return r;
  }
}

ssize_t fifo_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count) {
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    ssize_t r = fifo_write(ofd->fifo_channel(), buf, count, flags);
    if (r >= 0)
      return r;
    if (r == -EPIPE)
      raise_sigpipe_if_needed();
    cancel_check();
    if (r == -EINTR && signal_state::should_restart_syscall()) {
      cancel_check();
      continue;
    }
    return r;
  }
}

bool fifo_release_aux_impl(OpenFileDescription *ofd) {
  fifo_close_channel(ofd->fifo_channel());
  return true;
}

// --- SocketPair read/write ---

ssize_t sp_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  auto *sp = ofd->socket_pair();
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    ssize_t r = fifo_read(sp->read_ch, buf, count, flags);
    if (r >= 0)
      return r;
    cancel_check();
    if (r == -EINTR && signal_state::should_restart_syscall()) {
      cancel_check();
      continue;
    }
    return r;
  }
}

ssize_t sp_write_impl(OpenFileDescription *ofd, const void *buf,
                       size_t count) {
  auto *sp = ofd->socket_pair();
  uint8_t shut = sp->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & SP_SHUT_WR) {
    raise_sigpipe_if_needed();
    return -EPIPE;
  }
  int flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    ssize_t r = fifo_write(sp->write_ch, buf, count, flags);
    if (r >= 0)
      return r;
    if (r == -EPIPE)
      raise_sigpipe_if_needed();
    cancel_check();
    if (r == -EINTR && signal_state::should_restart_syscall()) {
      cancel_check();
      continue;
    }
    return r;
  }
}

bool sp_release_aux(OpenFileDescription *ofd) {
  sp_close_channel(ofd->socket_pair());
  return true;
}

// --- ConDrv (console) read/write ---

ssize_t condrv_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  return console_tty::read(ofd, buf, count);
}

ssize_t condrv_write_impl(OpenFileDescription *ofd, const void *buf,
                           size_t count) {
  return console_tty::write(ofd, buf, count);
}

// --- PTY read/write ---

ssize_t pty_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  return vt_pty::read(ofd, buf, count);
}

ssize_t pty_write_impl(OpenFileDescription *ofd, const void *buf,
                        size_t count) {
  return vt_pty::write(ofd, buf, count);
}

bool pty_master_release_aux(OpenFileDescription *ofd) {
  vt_pty::release_opaque(ofd->pty_session());
  return false; // Session owns the handle — caller must not NtClose.
}

bool pty_slave_release_aux(OpenFileDescription *ofd) {
  vt_pty::release_opaque(ofd->pty_session());
  return true;
}

// --- Inotify read ---

ssize_t inotify_read_impl(OpenFileDescription *ofd, void *buf, size_t count) {
  return inotify_read_ofd(ofd, buf, count);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
