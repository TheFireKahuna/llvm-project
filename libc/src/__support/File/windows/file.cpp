//===--- Implementation of the Windows specialization of File -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Disk files use the per-thread IO Ring (ioring::get_thread_ring()) for
// all I/O. When IO Ring is emulated (Hyper-V guests), falls back to
// FILE_SYNCHRONOUS_IO_ALERT handles.
//
// Non-disk handles (console, pipe, socket) use sync NtReadFile/NtWriteFile.
//
// File objects live in file_pool slots. platform_close cleans up NT
// resources but does NOT free the File object or fd_table entry.
//
// The IO Ring path:
//   ioring::push_* -> ioring::submit -> alertable NtWaitForSingleObject
//     STATUS_WAIT_0   -> ioring::pop_cqe -> done
//     STATUS_USER_APC -> cancel + wait for both CQEs -> return EINTR
//
//===----------------------------------------------------------------------===//

#include "file.h"

#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/stdio_macros.h"
#include "hdr/types/off_t.h"
#include "src/__support/CPP/new.h"
#include "src/__support/File/file.h"
#include "src/__support/OSUtil/windows/io/alertable_io.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_pool.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/op_tag.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/alloc-checker.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"

#include "include/llvm-libc-macros/sys-mman-macros.h" // PROT_*, MAP_*

#include "src/__support/CPP/new.h"

namespace LIBC_NAMESPACE_DECL {

static void raise_sigpipe_if_needed() {
  signal_state::generate_standard_signal_for_current_thread(SIGPIPE);
}

// Verify that File subclasses fit in the file_pool slot size.
static_assert(sizeof(IoRingFile) <= internal::file_pool::SLOT_SIZE,
              "IoRingFile exceeds file_pool::SLOT_SIZE — increase it");
static_assert(alignof(IoRingFile) <= internal::file_pool::SLOT_ALIGN,
              "IoRingFile alignment exceeds file_pool::SLOT_ALIGN");
static_assert(sizeof(WindowsFile) <= internal::file_pool::SLOT_SIZE,
              "WindowsFile exceeds file_pool::SLOT_SIZE");

//===----------------------------------------------------------------------===//
// Sync callbacks (non-ring handles: console, pipe, sync disk fallback)
//===----------------------------------------------------------------------===//

FileIOResult WindowsFile::platform_write(File *f, const void *data, size_t size) {
  auto *wf = static_cast<WindowsFile *>(f);

  // Resolve handle via fd table — dup2 may have redirected this fd.
  HANDLE h = wf->resolve_handle();
  if (!h)
    return {0, EBADF};

  // For append mode, pass the write-to-EOF sentinel so NT atomically seeks
  // to the end before writing. Without this, "a+" mode (which has both
  // FILE_APPEND_DATA and FILE_WRITE_DATA) would write at the current
  // position instead of EOF.
  LARGE_INTEGER append_offset;
  append_offset.LowPart = FILE_WRITE_TO_END_OF_FILE;
  append_offset.HighPart = -1;
  LARGE_INTEGER *byte_offset = wf->is_append_mode() ? &append_offset : nullptr;

  SyscallFrameGuard guard(h);

  for (;;) {
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status =
        NtWriteFile(h, wf->event, nullptr, nullptr, &iosb,
                    const_cast<void *>(data), static_cast<ULONG>(size),
                    byte_offset, nullptr);
    if (status == STATUS_PENDING) {
      status = alertable_wait_restartable(wf->event);
      if (status == STATUS_USER_APC) {
        IO_STATUS_BLOCK cancel_iosb = {};
        NtCancelIoFileEx(h, &iosb, &cancel_iosb);
        return {0, EINTR};
      }
      status = iosb.Status;
    }
    // Sync alertable handles return STATUS_ALERTED directly.
    if (status == STATUS_ALERTED) {
      if (signal_state::should_restart_syscall())
        continue;
      return {0, EINTR};
    }
    if (status == STATUS_PIPE_BROKEN || status == STATUS_PIPE_DISCONNECTED) {
      raise_sigpipe_if_needed();
      return {0, EPIPE};
    }
    if (!NT_SUCCESS(status))
      return {0, EIO};
    // Zero bytes written: non-blocking pipe with full buffer -> EAGAIN.
    if (iosb.Information == 0 && size > 0) {
      auto *ofd = internal::fd_table.get_ofd(wf->get_fd());
      if (ofd && (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK))
        return {0, EAGAIN};
    }
    return static_cast<size_t>(iosb.Information);
  }
}

FileIOResult WindowsFile::platform_read(File *f, void *buf, size_t size) {
  auto *wf = static_cast<WindowsFile *>(f);

  // Resolve handle via fd table — dup2 may have redirected this fd.
  HANDLE h = wf->resolve_handle();
  if (!h)
    return {0, EBADF};

  SyscallFrameGuard guard(h);

  for (;;) {
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status =
        NtReadFile(h, wf->event, nullptr, nullptr, &iosb,
                   buf, static_cast<ULONG>(size), nullptr, nullptr);
    if (status == STATUS_PENDING) {
      status = alertable_wait_restartable(wf->event);
      if (status == STATUS_USER_APC) {
        IO_STATUS_BLOCK cancel_iosb = {};
        NtCancelIoFileEx(h, &iosb, &cancel_iosb);
        return {0, EINTR};
      }
      status = iosb.Status;
    }
    // Sync alertable handles return STATUS_ALERTED directly.
    if (status == STATUS_ALERTED) {
      if (signal_state::should_restart_syscall())
        continue;
      return {0, EINTR};
    }
    if (status == STATUS_END_OF_FILE || status == STATUS_PIPE_BROKEN)
      return {0, 0};
    if (status == STATUS_PIPE_EMPTY)
      return {0, EAGAIN};
    if (!NT_SUCCESS(status))
      return {0, EIO};
    // Zero bytes read: EOF on blocking fds, EAGAIN on non-blocking pipes.
    if (iosb.Information == 0 && size > 0) {
      auto *ofd = internal::fd_table.get_ofd(wf->get_fd());
      if (ofd && (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK))
        return {0, EAGAIN};
    }
    return static_cast<size_t>(iosb.Information);
  }
}

// Map NT seek failures to POSIX errno. Pipes and consoles are not seekable;
// NT returns STATUS_INVALID_PARAMETER for position queries on such handles.
static int seek_ntstatus_to_errno(NTSTATUS status) {
  if (status == STATUS_INVALID_PARAMETER ||
      status == STATUS_INVALID_DEVICE_REQUEST)
    return ESPIPE;
  return EIO;
}

ErrorOr<off_t> WindowsFile::platform_seek(File *f, off_t offset, int whence) {
  auto *wf = static_cast<WindowsFile *>(f);

  // Resolve handle via fd table — dup2 may have redirected this fd.
  HANDLE h = wf->resolve_handle();
  if (!h)
    return Error(EBADF);

  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER new_offset;

  if (whence == SEEK_SET) {
    new_offset.QuadPart = offset;
  } else if (whence == SEEK_CUR) {
    FILE_POSITION_INFORMATION pos_info;
    NTSTATUS status = NtQueryInformationFile(
        h, &iosb, &pos_info,
        static_cast<ULONG>(sizeof(pos_info)), FilePositionInformation);
    if (!NT_SUCCESS(status))
      return Error(seek_ntstatus_to_errno(status));
    new_offset.QuadPart = pos_info.CurrentByteOffset.QuadPart + offset;
  } else if (whence == SEEK_END) {
    FILE_STANDARD_INFORMATION std_info;
    NTSTATUS status = NtQueryInformationFile(
        h, &iosb, &std_info,
        static_cast<ULONG>(sizeof(std_info)), FileStandardInformation);
    if (!NT_SUCCESS(status))
      return Error(seek_ntstatus_to_errno(status));
    new_offset.QuadPart = std_info.EndOfFile.QuadPart + offset;
  } else {
    return Error(EINVAL);
  }

  if (new_offset.QuadPart < 0)
    return Error(EINVAL);

  FILE_POSITION_INFORMATION set_pos;
  set_pos.CurrentByteOffset = new_offset;
  NTSTATUS status = NtSetInformationFile(
      h, &iosb, &set_pos, static_cast<ULONG>(sizeof(set_pos)),
      FilePositionInformation);
  if (!NT_SUCCESS(status))
    return Error(seek_ntstatus_to_errno(status));

  return static_cast<off_t>(new_offset.QuadPart);
}

// Close NT resources only — handle + event. Used by freopen (which
// reconstructs the File in-place) and by the full platform_close below.
int WindowsFile::close_nt_resources() {
  if (event)
    NtClose(event);
  NTSTATUS status = NtClose(handle);
  return NT_SUCCESS(status) ? 0 : EIO;
}

// Closes NT resources, releases the fd_table entry, and returns the pool
// slot. File::close() already called remove_file() before we get here.
int WindowsFile::platform_close(File *f) {
  auto *wf = static_cast<WindowsFile *>(f);
  int fd = wf->get_fd();

  int err = 0;
  if (fd >= 0) {
    auto *ofd = internal::fd_table.get_ofd(fd);
    if (ofd && ofd->handle == wf->handle) {
      // Normal case: FILE's cached handle matches the fd table entry.
      // Close NT resources and use free_slot (which nulls the handle in
      // the OFD before release to prevent double-close).
      err = wf->close_nt_resources();
      internal::fd_table.free_slot(fd);
    } else {
      // dup2 case: the fd now points to a different OFD. The original
      // handle was already closed when dup2 released the old OFD. Only
      // close the event (FILE-owned resource) and release the current
      // fd via the normal path so the replacement OFD is properly
      // refcount-decremented.
      if (wf->event)
        NtClose(wf->event);
      internal::fd_table.release(fd);
    }
  }

  // Return pool slot (equivalent to `delete lf` on Linux).
  internal::file_pool::free(wf);

  return err;
}

//===----------------------------------------------------------------------===//
// IO Ring read pipeline
//===----------------------------------------------------------------------===//
//
// The pipeline keeps PIPELINE_DEPTH reads in flight. Each platform_read
// pops the next completed CQE (typically instant — data arrived while the
// app was consuming the previous buffer), copies to the caller, and queues
// a refill SQE. Refill SQEs are batched: SUBMIT_BATCH refills accumulate
// before a single ioring::submit call. This gives us:
//   - 4 I/Os in flight -> saturates NVMe queues
//   - ~0.5 kernel calls per buffer refill (batched submission)
//   - Zero-wait reads in steady state (CQEs arrive before consumption)
//   - Zero per-op page pinning (registered buffers)

// Page-aligned allocation via our own mmap (NtAllocateVirtualMemoryEx internally).
static uint8_t *mmap_alloc(size_t size) {
  void *p = LIBC_NAMESPACE::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED)
    return nullptr;
  return static_cast<uint8_t *>(p);
}

static void mmap_free(void *ptr, size_t size) {
  if (ptr)
    LIBC_NAMESPACE::munmap(ptr, size);
}

bool IoRingFile::alloc_pipeline_bufs() {
  if (pipeline_region)
    return true;
  pipeline_region =
      mmap_alloc(static_cast<size_t>(PIPELINE_DEPTH) * IORING_BUFFER_SIZE);
  if (!pipeline_region)
    return false;
  for (int i = 0; i < PIPELINE_DEPTH; ++i) {
    pipeline[i].bytes = 0;
    pipeline[i].pending = false;
  }
  return true;
}

void IoRingFile::free_pipeline_bufs() {
  mmap_free(pipeline_region,
            static_cast<size_t>(PIPELINE_DEPTH) * IORING_BUFFER_SIZE);
  pipeline_region = nullptr;
}

// Submit PIPELINE_DEPTH read SQEs starting at the current position,
// then flush them all with one ioring::submit.
void IoRingFile::start_pipeline() {
  if (is_append_mode() || pipe_eof)
    return;
  if (!alloc_pipeline_bufs())
    return;

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return;

  pipe_head = 0;
  pipe_tail = 0;
  pipe_count = 0;
  pipe_pending_sqes = 0;
  pipe_next_offset = get_position();
  pipe_active = true;
  pipe_eof = false;

  for (int i = 0; i < PIPELINE_DEPTH; ++i) {
    ULONGLONG tag = static_cast<ULONGLONG>(i); // slot index as tag
    if (!push_pipeline_read_sqe(&tr->ring, i,
                                static_cast<ULONGLONG>(pipe_next_offset),
                                tag)) {
      pipe_active = false;
      return;
    }
    pipeline[i].offset = pipe_next_offset;
    pipeline[i].bytes = 0;
    pipeline[i].pending = true;
    pipe_next_offset += IORING_BUFFER_SIZE;
    pipe_tail = (i + 1) % PIPELINE_DEPTH;
    ++pipe_count;
  }

  // One kernel call for all PIPELINE_DEPTH reads.
  ioring::submit(&tr->ring);
}

// Cancel all in-flight pipeline SQEs and drain CQEs.
void IoRingFile::drain_pipeline() {
  if (!pipe_active) return;

  auto *tr = ioring::get_thread_ring();

  // Drain all CQEs — completed and cancel confirmations.
  NT_IORING_CQE drain;
  if (tr) {
    while (ioring::pop_cqe(&tr->ring, &drain))
      ;
  }

  for (int i = 0; i < PIPELINE_DEPTH; ++i) {
    pipeline[i].bytes = 0;
    pipeline[i].pending = false;
  }
  pipe_head = pipe_tail = pipe_count = pipe_pending_sqes = 0;
  pipe_active = false;
  pipe_eof = false;
  write_dirty = false;
}

// Pop the head slot's CQE and copy data to the caller's buffer.
// Returns true if data was served, false if the pipeline can't serve.
bool IoRingFile::pop_pipeline_slot(void *buf, size_t size, FileIOResult &out) {
  if (!pipe_active || pipe_count == 0)
    return false;

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return false;

  PipelineSlot &slot = pipeline[pipe_head];

  // Route a CQE to the correct pipeline slot by its UserData tag.
  auto route_cqe = [&](const NT_IORING_CQE &cqe) {
    int idx = static_cast<int>(cqe.UserData);
    if (idx < 0 || idx >= PIPELINE_DEPTH)
      return;
    pipeline[idx].pending = false;
    if (cqe.ResultCode >= 0)
      pipeline[idx].bytes = static_cast<size_t>(cqe.Information);
    else
      pipeline[idx].bytes = 0;
    // Short or zero read means we've reached/passed EOF. Stop refilling —
    // NT IoRing may not post CQEs for reads entirely past the file end.
    if (cqe.Information < IORING_BUFFER_SIZE)
      pipe_eof = true;
  };

  // Serve directly if CQE was already popped in a previous drain.
  if (!slot.pending && slot.bytes > 0 && slot.offset == get_position()) {
    size_t n = slot.bytes < size ? slot.bytes : size;
    __builtin_memcpy(buf, pipeline_buf(pipe_head), n);
    advance_position(static_cast<int64_t>(n));
    slot.bytes = 0;
    pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
    --pipe_count;
    refill_pipeline_slot();
    out = FileIOResult{n};
    return true;
  }

  // Slot is pending — drain all available CQEs (handles out-of-order
  // completions by routing each CQE to its slot by tag).
  if (slot.pending) {
    NT_IORING_CQE cqe;
    while (ioring::pop_cqe(&tr->ring, &cqe))
      route_cqe(cqe);

    // Head slot may now be ready after draining.
    if (!slot.pending && slot.bytes > 0 && slot.offset == get_position()) {
      size_t n = slot.bytes < size ? slot.bytes : size;
      __builtin_memcpy(buf, pipeline_buf(pipe_head), n);
      advance_position(static_cast<int64_t>(n));
      slot.bytes = 0;
      pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
      --pipe_count;
      refill_pipeline_slot();
      out = FileIOResult{n};
      return true;
    }

    // Head slot resolved as EOF (0 bytes read — past end of file).
    if (!slot.pending && slot.bytes == 0) {
      pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
      --pipe_count;
      out = FileIOResult{0, 0};
      return true;
    }

    // If pipe_eof is set (a prior slot returned short), this slot is likely
    // past EOF and its CQE may never arrive. Fall through to cold start.
    if (slot.pending && pipe_eof) {
      drain_pipeline();
      return false;
    }

    // Head slot still pending — alertable wait using thread ring event.
    if (slot.pending) {
      HANDLE evt = tr->event;
      if (!evt) {
        out = FileIOResult{0, EIO};
        return true;
      }
      SyscallFrameGuard pguard(handle);
      for (;;) {
        NTSTATUS status = alertable_wait_restartable(evt);
        if (status == STATUS_USER_APC) {
          drain_pipeline();
          out = FileIOResult{0, EINTR};
          return true;
        }
        if (status == STATUS_SUCCESS) {
          // Pop all available CQEs — may include multiple pipeline slots.
          NT_IORING_CQE cqe2;
          while (ioring::pop_cqe(&tr->ring, &cqe2))
            route_cqe(cqe2);
          if (!slot.pending)
            break;
          // Spurious wake — retry.
        }
      }

      // Head slot should now be ready.
      if (slot.bytes > 0 && slot.offset == get_position()) {
        size_t n = slot.bytes < size ? slot.bytes : size;
        __builtin_memcpy(buf, pipeline_buf(pipe_head), n);
        advance_position(static_cast<int64_t>(n));
        slot.bytes = 0;
        pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
        --pipe_count;
        refill_pipeline_slot();
        out = FileIOResult{n};
        return true;
      }

      // Head slot was EOF (0 bytes).
      if (slot.bytes == 0 && !slot.pending) {
        pipe_head = (pipe_head + 1) % PIPELINE_DEPTH;
        --pipe_count;
        out = FileIOResult{0, 0};
        return true;
      }
    }
  }

  // Offset mismatch (seek happened but pipeline wasn't drained).
  drain_pipeline();
  return false;
}

// Reuse the most recently consumed slot for the next sequential read.
// Builds the SQE but defers ioring::submit until SUBMIT_BATCH accumulates.
void IoRingFile::refill_pipeline_slot() {
  if (pipe_eof || pipe_count >= PIPELINE_DEPTH)
    return;

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return;

  // Reuse the slot that was just freed (pipe_head was advanced, so the
  // slot before head is the one we can reuse — which is pipe_tail).
  int slot_idx = pipe_tail;
  PipelineSlot &slot = pipeline[slot_idx];

  ULONGLONG tag = static_cast<ULONGLONG>(slot_idx);
  if (!push_pipeline_read_sqe(&tr->ring, slot_idx,
                              static_cast<ULONGLONG>(pipe_next_offset), tag))
    return;

  slot.offset = pipe_next_offset;
  slot.bytes = 0;
  slot.pending = true;
  pipe_next_offset += IORING_BUFFER_SIZE;
  pipe_tail = (slot_idx + 1) % PIPELINE_DEPTH;
  ++pipe_count;
  ++pipe_pending_sqes;

  // Batched submission: only call ioring::submit every SUBMIT_BATCH refills.
  if (pipe_pending_sqes >= SUBMIT_BATCH)
    flush_pending_submits();
}

void IoRingFile::flush_pending_submits() {
  if (pipe_pending_sqes > 0) {
    auto *tr = ioring::get_thread_ring();
    if (tr)
      ioring::submit(&tr->ring);
    pipe_pending_sqes = 0;
  }
}

void IoRingFile::free_file_buffer() {
  for (int i = 0; i < 2; ++i) {
    if (write_bufs[i])
      mmap_free(write_bufs[i], file_buf_size);
    write_bufs[i] = nullptr;
  }
  file_buf_size = 0;
}

bool IoRingFile::ensure_double_buffer() {
  if (write_bufs[1])
    return true;
  write_bufs[1] = mmap_alloc(file_buf_size);
  return write_bufs[1] != nullptr;
}

int IoRingFile::drain_write_behind() {
  if (!write_in_flight)
    return 0;

  auto *tr = ioring::get_thread_ring();
  if (!tr) {
    write_in_flight = false;
    return EIO;
  }

  // Pop the CQE for the in-flight write. Fast path: the CQE is usually
  // already in the shared memory ring (the write completed while the
  // application was filling the next buffer).
  NT_IORING_CQE cqe = {};
  bool got_cqe = ioring::pop_cqe(&tr->ring, &cqe);
  if (!got_cqe) {
    // CQE not ready — alertable wait using thread ring event.
    HANDLE evt = tr->event;
    if (!evt) {
      write_in_flight = false;
      return EIO;
    }
    SyscallFrameGuard wguard(handle);
    for (;;) {
      NTSTATUS ws = alertable_wait_restartable(evt);
      if (ws == STATUS_USER_APC) {
        write_in_flight = false;
        return EINTR;
      }
      if (ioring::pop_cqe(&tr->ring, &cqe))
        break;
      // Spurious wake — retry.
    }
  }

  write_in_flight = false;
  if (cqe.ResultCode < 0)
    return cqe_status_to_errno(cqe.ResultCode);
  // Advance position by the bytes actually written.
  if (!is_append_mode())
    advance_position(static_cast<int64_t>(cqe.Information));
  write_dirty = true;
  return 0;
}

//===----------------------------------------------------------------------===//
// IO Ring callbacks (disk files)
//===----------------------------------------------------------------------===//

FileIOResult IoRingFile::platform_write(File *f, const void *data, size_t size) {
  auto *irf = static_cast<IoRingFile *>(f);

  // Writes invalidate the read pipeline — data may be stale.
  if (irf->pipe_active)
    irf->drain_pipeline();

  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return {0, EIO};

  // Drain any previous in-flight write before reusing its buffer.
  // On the fast path this is a userspace ioring::pop_cqe (zero syscalls).
  int drain_err = irf->drain_write_behind();
  if (drain_err) {
    if (drain_err == EPIPE)
      raise_sigpipe_if_needed();
    return {0, drain_err};
  }

  ULONG64 offset = irf->is_append_mode()
                        ? FILE_WRITE_TO_END_OF_FILE64
                        : static_cast<ULONG64>(irf->get_position());

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  if (!irf->push_write_sqe(&tr->ring, data, static_cast<ULONG>(size), offset,
                            tag.as_user_data()))
    return {0, EIO};

  // Submit the SQE. Try the fast-path CQE pop — if the write completed
  // inline (OS cache hit), we can return immediately without write-behind.
  auto result = ioring_submit(&tr->ring, gen);
  if (result.has_error() && result.error != EAGAIN) {
    if (result.error == EPIPE)
      raise_sigpipe_if_needed();
    return result;
  }

  if (result.error != EAGAIN) {
    // Fast path: write completed immediately.
    if (!irf->is_append_mode())
      irf->advance_position(static_cast<int64_t>(result.value));
    irf->write_dirty = true;
    return result;
  }

  // Slow path: write is in flight. Swap to the alternate buffer so the
  // File base class can continue filling data while the kernel writes.
  if (irf->ensure_double_buffer()) {
    int active = irf->write_active;
    int alt = 1 - active;
    irf->activate_write_buf(alt);
    irf->write_in_flight = true;
    irf->write_dirty = true;
    // Optimistically report success for the full size. The actual byte
    // count is reconciled when drain_write_behind pops the CQE. If the
    // kernel wrote fewer bytes (short write), the error surfaces on the
    // next flush — POSIX allows deferred error reporting for buffered I/O.
    if (!irf->is_append_mode())
      irf->advance_position(static_cast<int64_t>(size));
    return FileIOResult{size};
  }

  // Double buffer allocation failed — fall back to synchronous wait.
  HANDLE evt = tr->event;
  if (!evt)
    return {0, EIO};
  result = ioring_wait_cqe(&tr->ring, evt, tr->wait_event,
                           irf->get_handle(), gen);
  if (result.has_error()) {
    if (result.error == EPIPE)
      raise_sigpipe_if_needed();
    return result;
  }
  if (!irf->is_append_mode())
    irf->advance_position(static_cast<int64_t>(result.value));
  irf->write_dirty = true;
  return result;
}

FileIOResult IoRingFile::platform_read(File *f, void *buf, size_t size) {
  auto *irf = static_cast<IoRingFile *>(f);

  // Pipeline path: pop the next pre-completed CQE (zero-wait if ready).
  FileIOResult pipe_result(0);
  if (irf->pop_pipeline_slot(buf, size, pipe_result))
    return pipe_result;

  // Pipeline not active — cold start. Direct read + start pipeline.
  auto *tr = ioring::get_thread_ring();
  if (!tr)
    return {0, EIO};

  uint32_t gen = tr->next_generation();
  ioring::OpTag tag{gen, 0};
  if (!irf->push_read_sqe(&tr->ring, buf, static_cast<ULONG>(size),
                           static_cast<ULONGLONG>(irf->get_position()),
                           tag.as_user_data()))
    return {0, EIO};

  FileIOResult result = ioring_submit_and_wait(
      &tr->ring, tr->event, tr->wait_event, irf->get_handle(), gen);
  if (!result.has_error()) {
    if (result.value == 0)
      return {0, 0}; // EOF
    irf->advance_position(static_cast<int64_t>(result.value));
    // Only start the pipeline if we got a full buffer — a short read means
    // we're at or near EOF, and NT IoRing reads past EOF may not post CQEs,
    // which would cause the pipeline to hang waiting forever.
    if (result.value == size)
      irf->start_pipeline();
  }
  return result;
}

ErrorOr<off_t> IoRingFile::platform_seek(File *f, off_t offset, int whence) {
  auto *irf = static_cast<IoRingFile *>(f);

  // Seek invalidates the pipeline — positions are wrong.
  irf->drain_pipeline();
  irf->write_dirty = false;

  if (whence == SEEK_SET) {
    irf->set_position(static_cast<int64_t>(offset));
  } else if (whence == SEEK_CUR) {
    irf->set_position(irf->get_position() + static_cast<int64_t>(offset));
  } else if (whence == SEEK_END) {
    FILE_STANDARD_INFORMATION std_info;
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status = NtQueryInformationFile(
        irf->get_handle(), &iosb, &std_info,
        static_cast<ULONG>(sizeof(std_info)), FileStandardInformation);
    if (!NT_SUCCESS(status))
      return Error(EIO);
    irf->set_position(std_info.EndOfFile.QuadPart +
                      static_cast<int64_t>(offset));
  } else {
    return Error(EINVAL);
  }

  if (irf->get_position() < 0)
    return Error(EINVAL);
  return static_cast<off_t>(irf->get_position());
}

// Close NT resources only — drains pipelines, frees buffers, closes handle.
// Used by freopen for in-place reconstruction.
int IoRingFile::close_nt_resources() {
  int err = drain_write_behind();
  drain_pipeline();
  free_pipeline_bufs();
  free_file_buffer();
  NtClose(handle);
  return err;
}

// Drains async pipelines, closes NT handle, releases fd_table entry, and
// returns the pool slot. File::close() already called remove_file().
int IoRingFile::platform_close(File *f) {
  auto *irf = static_cast<IoRingFile *>(f);
  int fd = irf->get_fd();

  int err = irf->close_nt_resources();

  // Release fd_table entry. free_slot nulls the handle in the OFD before
  // calling OFD::release(), preventing a double-close.
  if (fd >= 0)
    internal::fd_table.free_slot(fd);

  // Return pool slot.
  internal::file_pool::free(irf);

  return err;
}

int IoRingFile::platform_sync(File *f) {
  return static_cast<IoRingFile *>(f)->drain_write_behind();
}

//===----------------------------------------------------------------------===//
// File opening — constructs File objects in fd_table slots
//===----------------------------------------------------------------------===//

ErrorOr<File *> openfile(const char *path, const char *mode) {
  using ModeFlags = File::ModeFlags;
  auto modeflags = File::mode_flags(mode);
  if (modeflags == 0)
    return Error(EINVAL);

  // Translate fopen mode to NT access and disposition.
  ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
  ULONG disposition;

  if (modeflags & ModeFlags(File::OpenMode::APPEND)) {
    access |= FILE_APPEND_DATA | FILE_READ_DATA;
    disposition = FILE_OPEN_IF;
    if (modeflags & ModeFlags(File::OpenMode::PLUS))
      access |= FILE_WRITE_DATA;
  } else if (modeflags & ModeFlags(File::OpenMode::WRITE)) {
    access |= FILE_WRITE_DATA;
    disposition = FILE_OVERWRITE_IF;
    if (modeflags & ModeFlags(File::OpenMode::PLUS))
      access |= FILE_READ_DATA;
  } else {
    access |= FILE_READ_DATA;
    disposition = FILE_OPEN;
    if (modeflags & ModeFlags(File::OpenMode::PLUS))
      access |= FILE_WRITE_DATA;
  }

  if (modeflags & ModeFlags(File::CreateType::EXCLUSIVE))
    disposition = FILE_CREATE;

  // Convert path to NT format and open the file.
  WCHAR path_buf[MAX_NT_PATH_WCHARS];
  size_t path_len = to_nt_path(path, path_buf, MAX_NT_PATH_WCHARS);
  if (path_len == 0)
    return Error(EINVAL);

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, path_buf, path_len);

  HANDLE handle;
  ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE;
  bool use_ring = !is_ioring_emulated();

  NTSTATUS status;
  if (use_ring) {
    status = open_overlapped(&oa, access, disposition, share, &handle);
  } else {
    // Emulated IO Ring — fall back to sync alertable handle.
    status = open_sync_alertable(&oa, access, disposition, share, &handle);
  }

  if (!NT_SUCCESS(status)) {
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == static_cast<NTSTATUS>(0xC000003A))
      return Error(ENOENT);
    if (status == STATUS_ACCESS_DENIED)
      return Error(EACCES);
    return Error(EIO);
  }

  bool append = modeflags & ModeFlags(File::OpenMode::APPEND);

  int open_flags = 0;
  if (modeflags & ModeFlags(File::OpenMode::READ))
    open_flags |= O_RDONLY;
  if (modeflags & ModeFlags(File::OpenMode::WRITE))
    open_flags |= O_WRONLY;
  if (append)
    open_flags |= O_APPEND;

  // fd_table.alloc creates the OFD entry for the handle.
  auto fd_result = internal::fd_table.alloc(handle, open_flags);
  if (!fd_result.has_value()) {
    NtClose(handle);
    return Error(fd_result.error());
  }
  int fd = fd_result.value();
  auto *ofd = internal::fd_table.get_ofd(fd);

  // Allocate a pool slot for the File object.
  void *slot = internal::file_pool::alloc();
  if (!slot) {
    internal::fd_table.release(fd);
    NtClose(handle);
    return Error(ENOMEM);
  }

  if (!use_ring) {
    // Non-ring file (console, sync fallback).
    AllocChecker ac;
    auto *buffer = new (ac) uint8_t[File::DEFAULT_BUFFER_SIZE];
    if (!ac) {
      internal::file_pool::free(slot);
      internal::fd_table.release(fd);
      NtClose(handle);
      return Error(ENOMEM);
    }
    auto *file = new (slot) WindowsFile(
        handle, buffer, File::DEFAULT_BUFFER_SIZE, _IOFBF, true, modeflags,
        fd);
    ofd->file_ptr = file;
    File::add_file(file);
    return file;
  }

  // Disk file with IO Ring (per-thread ring, created lazily on first I/O).
  // Page-aligned buffer via mmap. owned=false: File::close() won't delete
  // this — ioring_file_close frees it with munmap instead.
  uint8_t *buffer = mmap_alloc(IoRingFile::IORING_BUFFER_SIZE);
  if (!buffer) {
    internal::file_pool::free(slot);
    internal::fd_table.release(fd);
    NtClose(handle);
    return Error(ENOMEM);
  }

  auto *file = new (slot) IoRingFile(
      handle, buffer, IoRingFile::IORING_BUFFER_SIZE,
      _IOFBF, /* owned */ false, modeflags, fd);
  ofd->file_ptr = file;
  File::add_file(file);

  return file;
}

int get_fileno(File *f) {
  // Every File* on Windows is a WindowsFileBase* (either WindowsFile for
  // console or IoRingFile for disk). The fd field lives in the shared base.
  return static_cast<WindowsFileBase *>(f)->get_fd();
}

} // namespace LIBC_NAMESPACE_DECL
