//===-- Windows internal file operations -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for file operations on Windows. These implement
// Linux syscall semantics in userspace: positive value or 0 on success,
// -errno on failure. Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "file_ops.h"
#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/off_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/File/file_io_result.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/alertable_io.h"
#include "src/__support/OSUtil/windows/io/batch_engine.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/error_or.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/resource/rlimit_query.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Pipeline chunk size for large single-op I/O. Splitting large reads/writes
// into 64K chunks keeps the NVMe command queue busy with multiple outstanding
// requests — the pipeline drains CQEs and pushes replacements to maintain QD.
// Only applied to O_DIRECT files where device latency dominates; for cached
// I/O the kernel's readahead/writeback already coalesces internally.
static constexpr ULONG PIPELINE_CHUNK_SIZE = 65536;

// =========================================================================
// internal::dup3
// =========================================================================

intptr_t dup3(int oldfd, int newfd, int flags) {
  // dup3 differs from dup2: oldfd == newfd is EINVAL, not a no-op.
  if (oldfd == newfd)
    return -EINVAL;

  // Only O_CLOEXEC is valid.
  if (flags & ~O_CLOEXEC)
    return -EINVAL;

  int fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
  auto result = fd_table.dup_to(oldfd, newfd, fd_flags);
  if (!result.has_value())
    return -result.error();
  return newfd;
}

// =========================================================================
// internal::pread
// =========================================================================

intptr_t pread(int fd, void *buf, size_t count, off_t offset) {
  if (LIBC_UNLIKELY(count == 0))
    return 0;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (LIBC_UNLIKELY(ofd->is_path_only()))
    return -EBADF;

  // /dev/zero emulation: return zero-filled buffer.
  if (LIBC_UNLIKELY(ofd->is_dev_zero())) {
    __builtin_memset(buf, 0, count);
    return static_cast<intptr_t>(count);
  }

  if (LIBC_UNLIKELY(!ofd->is_seekable()))
    return -ESPIPE;

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;

  HANDLE h = ofd->handle;

  ULONG io_len = static_cast<ULONG>(count <= 0xFFFFFFFFULL ? count
                                                            : 0xFFFFFFFFU);

  int sflags = ofd->status_flags.load(cpp::MemoryOrder::RELAXED);

  // Large O_DIRECT reads: split into pipeline chunks to keep the NVMe
  // command queue busy. BatchEngine manages registered buffers (MDL
  // pre-pinning), pipeline submission, and batch amortization internally.
  if (LIBC_UNLIKELY((sflags & O_DIRECT) && io_len > PIPELINE_CHUNK_SIZE)) {
    ioring::BatchEngine batch;
    batch.init(tr, h, /*use_regbuf=*/true);

    char *p = static_cast<char *>(buf);
    ULONGLONG off = static_cast<ULONGLONG>(offset);
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
    if (br.error)
      return -br.error;
    return static_cast<intptr_t>(br.total_bytes);
  }

  // Small O_DIRECT reads: single SQE with registered buffer bounce.
  if (LIBC_UNLIKELY((sflags & O_DIRECT) && tr->ensure_reg_buf())) {
    uint32_t gen = tr->next_generation();
    FileIOResult result =
        ioring_pread_registered(tr, h, buf, io_len,
                                static_cast<ULONGLONG>(offset), gen);
    if (result.error != EAGAIN) {
      if (result.has_error())
        return -result.error;
      return static_cast<intptr_t>(result.value);
    }
  }

  // Cached I/O or registered buffer fallback: combined push+submit+pop.
  uint32_t gen = tr->next_generation();
  FileIOResult result = read_single_op_wait(tr, h, buf, io_len,
                                            static_cast<ULONGLONG>(offset),
                                            gen);
  if (result.has_error())
    return -result.error;
  // pread does NOT advance position.
  return static_cast<intptr_t>(result.value);
}

// =========================================================================
// internal::pwrite
// =========================================================================

intptr_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
  if (LIBC_UNLIKELY(count == 0))
    return 0;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (LIBC_UNLIKELY(ofd->is_path_only()))
    return -EBADF;

  // /dev/zero emulation: discard writes.
  if (LIBC_UNLIKELY(ofd->is_dev_zero()))
    return static_cast<intptr_t>(count);

  if (LIBC_UNLIKELY(!ofd->is_seekable()))
    return -ESPIPE;

  // RLIMIT_FSIZE: check if write at this offset would exceed the limit.
  rlim_t fsize = windows::get_fsize_limit();
  if (LIBC_UNLIKELY(fsize != RLIM_INFINITY &&
      static_cast<int64_t>(offset) + static_cast<int64_t>(count) >
          static_cast<int64_t>(fsize))) {
    signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
    return -EFBIG;
  }

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;

  HANDLE h = ofd->handle;

  ULONG io_len = static_cast<ULONG>(count <= 0xFFFFFFFFULL ? count
                                                            : 0xFFFFFFFFU);

  int sflags = ofd->status_flags.load(cpp::MemoryOrder::RELAXED);

  // Large O_DIRECT writes: pipeline chunks to keep NVMe queue busy.
  if (LIBC_UNLIKELY((sflags & O_DIRECT) && io_len > PIPELINE_CHUNK_SIZE)) {
    ioring::BatchEngine batch;
    batch.init(tr, h, /*use_regbuf=*/true);

    const char *p = static_cast<const char *>(buf);
    ULONGLONG off = static_cast<ULONGLONG>(offset);
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
    if (br.error)
      return -br.error;
    return static_cast<intptr_t>(br.total_bytes);
  }

  // Small O_DIRECT writes: registered bounce buffer avoids kernel probe.
  if (LIBC_UNLIKELY((sflags & O_DIRECT) && tr->ensure_reg_buf())) {
    uint32_t gen = tr->next_generation();
    FileIOResult result =
        ioring_pwrite_registered(tr, h, buf, io_len,
                                 static_cast<ULONGLONG>(offset), gen);
    if (result.error != EAGAIN) {
      if (result.has_error())
        return -result.error;
      return static_cast<intptr_t>(result.value);
    }
  }

  // Cached I/O: prefault pages (volatile reads, ~1 cyc/page) so the
  // kernel can probe the user buffer directly — avoids the double-copy
  // of a registered bounce and the ~8K cycle NtQueryVirtualMemory of
  // the old materialize_file_private_range.
  // Combined push+submit+pop for cached I/O.
  windows::prefault_read_pages(buf, io_len);
  uint32_t gen = tr->next_generation();
  FileIOResult result = write_single_op_wait(tr, h, buf, io_len,
                                             static_cast<ULONGLONG>(offset),
                                             gen);
  if (result.has_error())
    return -result.error;
  // pwrite does NOT advance position.
  return static_cast<intptr_t>(result.value);
}

// =========================================================================
// internal::fsync
// =========================================================================

intptr_t fsync(int fd) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  HANDLE h = ofd->handle;

  if (!ofd->is_seekable()) {
    // Non-disk fd (console, pipe): direct NT flush.
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status =
        NtFlushBuffersFileEx(h, FLUSH_FLAGS_FILE_NORMAL, nullptr, 0, &iosb);
    if (!NT_SUCCESS(status))
      return -windows_util::ntstatus_to_errno(status);
    return 0;
  }

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;

  ioring::RingState *ring = &tr->ring;

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  auto *sqe = ioring::push_flush(ring, h, tag.as_user_data());
  if (!sqe)
    return -EIO;

  FileIOResult result = ioring_submit_and_wait(tr, h, gen);
  if (result.has_error())
    return -result.error;
  return 0;
}

// =========================================================================
// internal::fdatasync
// =========================================================================

intptr_t fdatasync(int fd) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  HANDLE h = ofd->handle;

  if (!ofd->is_seekable()) {
    // Non-disk fd: direct NT flush (see fsync for rationale).
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status = NtFlushBuffersFileEx(
        h, FLUSH_FLAGS_FILE_DATA_SYNC_ONLY, nullptr, 0, &iosb);
    if (!NT_SUCCESS(status))
      return -windows_util::ntstatus_to_errno(status);
    return 0;
  }

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return -EIO;

  ioring::RingState *ring = &tr->ring;

  // Disk: IO Ring flush with data-sync-only (skip timestamps).
  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  auto *sqe = ioring::push_flush(ring, h, tag.as_user_data(),
                                 FLUSH_FLAGS_FILE_DATA_SYNC_ONLY);
  if (!sqe)
    return -EIO;

  FileIOResult result = ioring_submit_and_wait(tr, h, gen);
  if (result.has_error())
    return -result.error;
  return 0;
}

// =========================================================================
// internal::ftruncate
// =========================================================================

intptr_t ftruncate(int fd, off_t length) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  // RLIMIT_FSIZE: extending a file beyond the limit raises SIGXFSZ.
  rlim_t fsize = windows::get_fsize_limit();
  if (fsize != RLIM_INFINITY && length > static_cast<off_t>(fsize)) {
    signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
    return -EFBIG;
  }

  HANDLE h = ofd->handle;

  FILE_END_OF_FILE_INFORMATION eof_info;
  eof_info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = NtSetInformationFile(
      h, &iosb, &eof_info, static_cast<ULONG>(sizeof(eof_info)),
      FileEndOfFileInformation);
  if (!NT_SUCCESS(status)) {
    // POSIX ftruncate: EBADF/EINVAL if not open for writing. The kernel
    // returns STATUS_ACCESS_DENIED, but the POSIX error depends on
    // whether the fd is valid but read-only (EINVAL) vs truly bad.
    // Since we already validated the fd above, ACCESS_DENIED means
    // the fd isn't open for writing.
    if (status == STATUS_ACCESS_DENIED)
      return -EINVAL;
    return -windows_util::ntstatus_to_errno(status);
  }

  // Extend the pre-created section if one exists (memfd_create / shm_open).
  // The file's EOF is now updated, but the section's max size is a separate
  // metadata field that must be explicitly extended. Without this,
  // NtMapViewOfSectionEx would fail for views beyond the old section size.
  HANDLE sh = ofd->disk().section_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (sh && length > 0) {
    nt_helpers::extend_section(sh, static_cast<SIZE_T>(length));
    // NtExtendSection failure is non-fatal — the section may already be
    // large enough, or the kernel may track the file size automatically
    // for file-backed sections. mmap will report the error if the view
    // creation actually fails.
  }

  return 0;
}

// =========================================================================
// internal::truncate
// =========================================================================

intptr_t truncate(const char *path, off_t length) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  if (length < 0)
    return -EINVAL;
  string_view sv(path);

  // RLIMIT_FSIZE: truncate also must respect the file size limit.
  rlim_t fsize = windows::get_fsize_limit();
  if (fsize != RLIM_INFINITY && length > static_cast<off_t>(fsize)) {
    signal_state::generate_standard_signal_for_current_thread(SIGXFSZ);
    return -EFBIG;
  }

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  auto nt = to_nt_path(sv, path_buf, path_buf_s.size());
  if (!nt.has_value())
    return -nt.error();
  size_t path_len = nt.value();

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name(path_buf, path_len);
  init_object_attributes(&oa, &name);

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  FILE_END_OF_FILE_INFORMATION eof_info;
  eof_info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);

  status = ::NtSetInformationFile(handle.get(), &iosb, &eof_info,
                                  static_cast<ULONG>(sizeof(eof_info)),
                                  FileEndOfFileInformation);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  return 0;
}

// =========================================================================
// internal::isatty
// =========================================================================

intptr_t isatty(int fd) {
  int err = console_tty::is_terminal_fd(fd);
  if (err < 0)
    return err;
  return 1;
}

// =========================================================================
// internal::pipe2
// =========================================================================

intptr_t pipe2(int pipefd[2], int flags) {
  if (!pipefd)
    return -EFAULT;

  // Create an anonymous kernel pipe via NPFS. This produces two real NT
  // handles (server=read end, client=write end) that are inheritable via
  // NtDuplicateObject and work with IoRing, poll, and posix_spawn.
  //
  // The pipe is created under \Device\NamedPipe\ with an empty name
  // (ObjectAttributes.ObjectName = empty UNICODE_STRING, RootDirectory =
  // \Device\NamedPipe handle). This is how the NT kernel creates truly
  // anonymous pipes — no named object in the namespace.

  // Open the NPFS root directory for creating child pipe objects.
  static constexpr WCHAR npfs_root[] = u"\\Device\\NamedPipe\\";
  windows::nt_wstring_view npfs_name(npfs_root);

  auto root_oa = windows::named_internal_oa(&npfs_name);

  IO_STATUS_BLOCK iosb = {};
  HANDLE npfs_dir = nullptr;
  NTSTATUS status = NtOpenFile(
      &npfs_dir, SYNCHRONIZE | FILE_READ_ATTRIBUTES, &root_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Create the server (read) end with an empty name relative to the root.
  // Empty nt_wstring_view = anonymous pipe.
  windows::nt_wstring_view empty_name;

  auto pipe_oa = windows::named_oa(&empty_name, npfs_dir, /*inherit=*/true);

  HANDLE read_handle = nullptr;
  LARGE_INTEGER timeout;
  timeout.QuadPart = -1200000000LL; // 120 seconds (default, not actually used)

  iosb = {};
  status = NtCreateNamedPipeFile(
      &read_handle,
      FILE_GENERIC_READ | FILE_WRITE_ATTRIBUTES,
      &pipe_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_CREATE,          // Must not already exist (anonymous).
      0,                         // Overlapped I/O — IoRing handles async completion.
      FILE_PIPE_MESSAGE_TYPE,    // Kernel-guaranteed write atomicity up to quota.
      FILE_PIPE_BYTE_STREAM_MODE, // Reads see a continuous byte stream.
      FILE_PIPE_QUEUE_OPERATION,
      1,                    // MaximumInstances = 1 (anonymous pipe).
      65536,                // InboundQuota (read buffer size).
      65536,                // OutboundQuota (write buffer size).
      &timeout);

  NtClose(npfs_dir);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Open the client (write) end by opening the same unnamed pipe instance.
  // Re-open relative to the server handle itself — this is how NtCreatePipe
  // (the internal kernel API behind CreatePipe) connects the two ends.
  pipe_oa.RootDirectory = read_handle;

  HANDLE write_handle = nullptr;
  iosb = {};
  status = NtOpenFile(
      &write_handle,
      FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES,
      &pipe_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_NON_DIRECTORY_FILE); // Overlapped I/O — matches read end.

  if (!NT_SUCCESS(status)) {
    NtClose(read_handle);
    return -windows_util::ntstatus_to_errno(status);
  }

  // Allocate fds with real kernel handles. query_file_type returns
  // FILE_TYPE_PIPE (3), so is_seekable()=false and IoRing passes offset=0.
  bool nonblock = (flags & O_NONBLOCK) != 0;
  int rflags = O_RDONLY | (nonblock ? O_NONBLOCK : 0);
  int wflags = O_WRONLY | (nonblock ? O_NONBLOCK : 0);

  auto read_fd = fd_table.alloc(read_handle, rflags);
  if (!read_fd.has_value()) {
    NtClose(write_handle);
    NtClose(read_handle);
    return -read_fd.error();
  }

  auto write_fd = fd_table.alloc(write_handle, wflags);
  if (!write_fd.has_value()) {
    fd_table.release(read_fd.value());
    NtClose(write_handle);
    return -write_fd.error();
  }

  // Create event-only FifoChannels for poll/select/epoll notification.
  // The events are signaled/cleared by the read and write paths.
  {
    internal::FifoChannel *rch = nullptr;
    internal::FifoChannel *wch = nullptr;
    if (internal::fifo_create_pipe_events(&rch, &wch)) {
      auto *rofd = fd_table.get_ofd(read_fd.value());
      auto *wofd = fd_table.get_ofd(write_fd.value());
      if (rofd) rofd->set_pipe_events(rch);
      else internal::fifo_close_channel(rch);
      if (wofd) wofd->set_pipe_events(wch);
      else internal::fifo_close_channel(wch);
    }
    // If event creation fails, the pipe still works — just without
    // poll/epoll notification. This is a soft failure.
  }

  // Apply O_CLOEXEC — set slot bit and revoke OBJ_INHERIT on handles.
  if (flags & O_CLOEXEC) {
    fd_table.set_fd_cloexec(read_fd.value(), true);
    fd_table.set_fd_cloexec(write_fd.value(), true);
  }

  pipefd[0] = read_fd.value();
  pipefd[1] = write_fd.value();
  return 0;
}

// =========================================================================
// internal::copy_file_range
// =========================================================================

intptr_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                         size_t len, unsigned int flags) {
  // Linux requires flags == 0; no flags are defined.
  if (flags != 0)
    return -EINVAL;

  OpenFileDescription *ofd_in = fd_table.get_ofd(fd_in);
  OpenFileDescription *ofd_out = fd_table.get_ofd(fd_out);
  if (!ofd_in || !ofd_out)
    return -EBADF;
  if (ofd_in->is_path_only() || ofd_out->is_path_only())
    return -EBADF;

  // Both must be regular (seekable) files.
  if (!ofd_in->is_seekable() || !ofd_out->is_seekable())
    return -EINVAL;

  // Input must be readable, output must be writable.
  if ((ofd_in->access_mode & O_ACCMODE) == O_WRONLY)
    return -EBADF;
  if ((ofd_out->access_mode & O_ACCMODE) == O_RDONLY)
    return -EBADF;

  if (len == 0)
    return 0;

  // Reject same-OFD when both offsets are NULL — two fetch_add() calls on
  // the same position atomic would claim non-overlapping ranges, making the
  // destination write to wrong offsets. Linux 5.3+ also rejects same-file
  // copy_file_range with EINVAL.
  if (ofd_in == ofd_out && !off_in && !off_out)
    return -EINVAL;

  HANDLE h_in = ofd_in->handle;
  HANDLE h_out = ofd_out->handle;

  // Validate len fits in int64_t before casting. The check must precede the
  // cast to avoid undefined behavior (signed overflow) when len > INT64_MAX.
  if (len > static_cast<size_t>(INT64_MAX))
    return -EINVAL;
  int64_t slen = static_cast<int64_t>(len);

  // Validate explicit offsets before pre-claiming fd positions.
  if (off_in && *off_in < 0)
    return -EINVAL;
  if (off_out && *off_out < 0)
    return -EINVAL;

  // Validate that explicit offsets + len won't overflow int64_t.
  if (off_in && static_cast<uint64_t>(*off_in) >
                    static_cast<uint64_t>(INT64_MAX) - len)
    return -EINVAL;
  if (off_out && static_cast<uint64_t>(*off_out) >
                     static_cast<uint64_t>(INT64_MAX) - len)
    return -EINVAL;

  // Determine source offset. Pre-claim the position range when using the
  // fd position to avoid TOCTOU races with concurrent I/O on the same fd.
  int64_t src_pos;
  bool src_preclaimed = false;
  if (off_in) {
    src_pos = *off_in;
  } else {
    src_pos = ofd_in->disk().position.fetch_add(slen, cpp::MemoryOrder::ACQ_REL);
    src_preclaimed = true;
    // Validate the claimed range doesn't overflow.
    if (src_pos < 0 ||
        static_cast<uint64_t>(src_pos) >
            static_cast<uint64_t>(INT64_MAX) - len) {
      ofd_in->disk().position.fetch_sub(slen, cpp::MemoryOrder::ACQ_REL);
      return -EINVAL;
    }
  }

  // Determine destination offset. O_APPEND is ignored for copy_file_range,
  // consistent with Linux kernel behavior (the generic filesystem
  // implementation does not honor O_APPEND on the output fd).
  int64_t dst_pos;
  bool dst_preclaimed = false;
  if (off_out) {
    dst_pos = *off_out;
  } else {
    dst_pos = ofd_out->disk().position.fetch_add(slen, cpp::MemoryOrder::ACQ_REL);
    dst_preclaimed = true;
    if (dst_pos < 0 ||
        static_cast<uint64_t>(dst_pos) >
            static_cast<uint64_t>(INT64_MAX) - len) {
      ofd_out->disk().position.fetch_sub(slen, cpp::MemoryOrder::ACQ_REL);
      if (src_preclaimed)
        ofd_in->disk().position.fetch_sub(slen, cpp::MemoryOrder::ACQ_REL);
      return -EINVAL;
    }
  }

  // Scope guards: give back unused pre-claimed bytes on any exit path.
  // Captures total by reference — always reflects actual bytes at exit.
  // slen is guaranteed to be a valid int64_t (validated above).
  size_t total = 0;
  auto src_guard = cpp::make_scope_guard([&] {
    if (off_in) {
      *off_in += static_cast<off_t>(total);
    } else if (src_preclaimed) {
      int64_t unused = slen - static_cast<int64_t>(total);
      if (unused > 0)
        ofd_in->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
    }
  });
  auto dst_guard = cpp::make_scope_guard([&] {
    if (off_out) {
      *off_out += static_cast<off_t>(total);
    } else if (dst_preclaimed) {
      int64_t unused = slen - static_cast<int64_t>(total);
      if (unused > 0)
        ofd_out->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
    }
  });

  // Create event for NtCopyFileChunk completion.
  windows::ScopedNtHandle evt;
  auto oa = windows::internal_oa();
  NTSTATUS es = ::NtCreateEvent(evt.put(), EVENT_MODIFY_STATE | SYNCHRONIZE, &oa,
                                SynchronizationEvent, FALSE);
  if (!NT_SUCCESS(es))
    return -EIO;

  intptr_t error_result = 0;

  while (total < len) {
    // NtCopyFileChunk Length is ULONG (32-bit). Chunk to avoid overflow.
    ULONG chunk = static_cast<ULONG>(
        (len - total) > 0xFFFFFFFFULL ? 0xFFFFFFFFU
                                      : static_cast<ULONG>(len - total));

    LARGE_INTEGER src_offset, dst_offset;
    src_offset.QuadPart = src_pos + static_cast<int64_t>(total);
    dst_offset.QuadPart = dst_pos + static_cast<int64_t>(total);

    IO_STATUS_BLOCK iosb = {};
    NTSTATUS s = ::NtCopyFileChunk(h_in, h_out, evt.get(), &iosb, chunk,
                                   &src_offset, &dst_offset,
                                   nullptr, nullptr, 0);
    if (s == STATUS_PENDING) {
      s = alertable_wait_restartable(evt.get());
      if (s == STATUS_USER_APC) {
        // Signal interrupted, not restartable.
        if (total == 0)
          error_result = -EINTR;
        break;
      }
      if (NT_SUCCESS(s))
        s = iosb.Status;
    }

    if (!NT_SUCCESS(s)) {
      if (total == 0) {
        if (s == STATUS_END_OF_FILE)
          error_result = 0;
        else
          error_result = -windows_util::ntstatus_to_errno(s);
      }
      break;
    }

    size_t copied = static_cast<size_t>(iosb.Information);
    if (copied == 0)
      break; // EOF

    total += copied;

    // Short copy — stop (remainder would be past EOF).
    if (copied < chunk)
      break;
  }

  // Scope guards handle offset/position updates on return.
  if (total > 0)
    return static_cast<intptr_t>(total);
  return error_result;
}

// =========================================================================
// internal::get_osfhandle / internal::open_osfhandle
// =========================================================================

intptr_t get_osfhandle(int fd) {
  auto result = fd_table.get(fd);
  if (!result)
    return -EBADF;
  return reinterpret_cast<intptr_t>(*result);
}

intptr_t open_osfhandle(intptr_t osfhandle, int flags) {
  HANDLE h = reinterpret_cast<HANDLE>(osfhandle);
  auto result = fd_table.alloc(h, flags);
  if (!result.has_value())
    return -result.error();
  return result.value();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
