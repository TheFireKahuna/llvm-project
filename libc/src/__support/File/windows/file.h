//===--- Windows specialization of the File data structure -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_FILE_WINDOWS_FILE_H
#define LLVM_LIBC_SRC___SUPPORT_FILE_WINDOWS_FILE_H

#include "hdr/types/off_t.h"
#include "src/__support/File/file.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_ops.h"
#include "src/__support/OSUtil/windows/io/thread_ring.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// WindowsFileBase — shared base for all Windows File subclasses
//===----------------------------------------------------------------------===//

class WindowsFileBase : public File {
  int fd;

protected:
  constexpr WindowsFileBase(WriteFunc *wf, ReadFunc *rf, SeekFunc *sf,
                            CloseFunc *cf, uint8_t *buffer, size_t buffer_size,
                            int buffer_mode, bool owned,
                            File::ModeFlags modeflags, int file_descriptor,
                            SyncFunc *syncf = nullptr)
      : File(wf, rf, sf, cf, buffer, buffer_size, buffer_mode, owned,
             modeflags, syncf),
        fd(file_descriptor) {}

public:
  int get_fd() const { return fd; }
  void set_fd(int file_descriptor) { fd = file_descriptor; }

  // Resolve the current NT handle for this FILE's fd via the fd table.
  // After dup2, this returns the redirected handle, not the stale cached one.
  HANDLE resolve_handle() const {
    if (fd >= 0) {
      if (auto *ofd = internal::fd_table.get_ofd(fd))
        return ofd->handle;
    }
    return nullptr;
  }

  // Used by freopen(NULL, mode, stream) on non-seekable handles (consoles)
  // where the handle can't be reopened by path — just update mode flags.
  // C11 §7.21.5.4: freopen with NULL changes the mode of the stream.
  void set_mode(ModeFlags mf) { set_mode_flags(mf); }
};

//===----------------------------------------------------------------------===//
// WindowsFile — sync NtReadFile/NtWriteFile (console, pipe, sync disk)
//===----------------------------------------------------------------------===//
//
// Handles synchronous handles (console from GetStdHandle, sync disk when IO
// Ring is emulated) and overlapped handles (pipes from open()). The event is
// used for alertable waits on overlapped handles; nullptr for synchronous
// handles (NtReadFile blocks inline, returning STATUS_ALERTED for EINTR).

class WindowsFile : public WindowsFileBase {
  HANDLE handle;
  HANDLE event; // Completion event for overlapped handles; nullptr for sync.

  // Platform callbacks — static members with private access to fields.
  static FileIOResult platform_write(File *, const void *, size_t);
  static FileIOResult platform_read(File *, void *, size_t);
  static ErrorOr<off_t> platform_seek(File *, off_t, int);
  static int platform_close(File *);

public:
  constexpr WindowsFile(HANDLE file_handle, uint8_t *buffer,
                        size_t buffer_size, int buffer_mode, bool owned,
                        File::ModeFlags modeflags, int file_descriptor = -1,
                        HANDLE completion_event = nullptr)
      : WindowsFileBase(&WindowsFile::platform_write,
                         &WindowsFile::platform_read,
                         &WindowsFile::platform_seek,
                         &WindowsFile::platform_close, buffer,
                         buffer_size, buffer_mode, owned, modeflags,
                         file_descriptor),
        handle(file_handle), event(completion_event) {}

  HANDLE get_handle() const { return handle; }

  // Close NT resources only (handle + event). Does NOT free the pool slot
  // or release the fd_table entry. Used by freopen which reconstructs the
  // File in-place at the same address.
  int close_nt_resources();
};

//===----------------------------------------------------------------------===//
// IoRingFile — IO Ring file with depth-N read pipeline
//===----------------------------------------------------------------------===//
//
// Uses the per-thread IO Ring (ioring::get_thread_ring()) for all I/O.
// The ring is shared across all IoRingFiles on the same thread.
//
// Read pipeline: N read SQEs are kept in flight at all times. Each
// platform_read pops the next CQE (likely already completed — zero wait),
// copies data to the File buffer, and refills the consumed slot. SQE
// refills are batched: submitted every SUBMIT_BATCH reads to minimize
// kernel transitions.
//
// Write path: double-buffered write-behind. The File buffer is flushed
// via IO Ring write SQE, then swapped to the alternate buffer so the
// application can continue filling while the kernel writes.
//
// The pipeline is invisible to the File base class — platform_read
// appears synchronous.

class IoRingFile : public WindowsFileBase {
  HANDLE handle;

  // Double-buffered write-behind. Two mmap'd page-aligned buffers swap
  // roles: one is the active FILE buffer (base class writes into it),
  // the other is the in-flight buffer (kernel reads from it via IO Ring).
  uint8_t *write_bufs[2];
  size_t file_buf_size;
  int write_active;
  bool write_in_flight;

  // Read pipeline — circular buffer of N slots backed by a single mmap.
  static constexpr int PIPELINE_DEPTH = 4;
  static constexpr int SUBMIT_BATCH = 2;

  struct PipelineSlot {
    int64_t offset;
    size_t bytes;
    bool pending;
  };

  uint8_t *pipeline_region;
  PipelineSlot pipeline[PIPELINE_DEPTH];
  int pipe_head;
  int pipe_tail;
  int pipe_count;
  int pipe_pending_sqes;
  int64_t pipe_next_offset;
  bool pipe_active;
  bool pipe_eof;
  bool write_dirty;

  // Platform callbacks — static members with private access to fields.
  static FileIOResult platform_write(File *, const void *, size_t);
  static FileIOResult platform_read(File *, void *, size_t);
  static ErrorOr<off_t> platform_seek(File *, off_t, int);
  static int platform_close(File *);
  static int platform_sync(File *);

  // Position management via OFD (single source of truth for dup sharing).
  int64_t get_position() const {
    auto *ofd = internal::fd_table.get_ofd(get_fd());
    if (!ofd || ofd->kind != internal::FileKind::Disk)
      return 0;
    return ofd->disk().position.load(cpp::MemoryOrder::ACQUIRE);
  }
  void set_position(int64_t pos) {
    auto *ofd = internal::fd_table.get_ofd(get_fd());
    if (ofd && ofd->kind == internal::FileKind::Disk)
      ofd->disk().position.store(pos, cpp::MemoryOrder::RELEASE);
  }
  void advance_position(int64_t delta) {
    auto *ofd = internal::fd_table.get_ofd(get_fd());
    if (ofd && ofd->kind == internal::FileKind::Disk)
      ofd->disk().position.fetch_add(delta, cpp::MemoryOrder::ACQ_REL);
  }

  // Pipeline internals.
  uint8_t *pipeline_buf(int idx) const {
    return pipeline_region + idx * IORING_BUFFER_SIZE;
  }
  bool alloc_pipeline_bufs();
  void free_pipeline_bufs();

  // IO Ring SQE helpers — thin wrappers that pass handle to ioring::push_*.
  bool push_read_sqe(ioring::RingState *ring, void *buf, ULONG size,
                     ULONGLONG offset, ULONGLONG tag) {
    return ioring::push_read(ring, handle, buf, size, offset, tag) != nullptr;
  }
  bool push_write_sqe(ioring::RingState *ring, const void *data, ULONG size,
                      ULONGLONG offset, ULONGLONG tag) {
    return ioring::push_write(ring, handle, data, size, offset, tag) != nullptr;
  }
  bool push_pipeline_read_sqe(ioring::RingState *ring, int slot_idx,
                               ULONGLONG offset, ULONGLONG tag) {
    return ioring::push_read(ring, handle, pipeline_buf(slot_idx),
                             static_cast<ULONG>(IORING_BUFFER_SIZE), offset,
                             tag) != nullptr;
  }
  void start_pipeline();
  void drain_pipeline();
  bool pop_pipeline_slot(void *buf, size_t size, FileIOResult &out);
  void refill_pipeline_slot();
  void flush_pending_submits();

  // Write-behind internals.
  void free_file_buffer();
  int drain_write_behind();
  bool ensure_double_buffer();
  void activate_write_buf(int idx) {
    set_buffer_ptr(write_bufs[idx]);
    write_active = idx;
  }

public:
  // 16KB per pipeline slot (64KB total in flight) — better saturates NVMe
  // queue depth than 4KB while staying within reasonable memory use.
  static constexpr size_t IORING_BUFFER_SIZE = 16384;

  HANDLE get_handle() const { return handle; }

  IoRingFile(HANDLE h, uint8_t *buffer, size_t buffer_size, int buffer_mode,
             bool owned, File::ModeFlags modeflags,
             int file_descriptor = -1)
      : WindowsFileBase(&IoRingFile::platform_write,
                         &IoRingFile::platform_read,
                         &IoRingFile::platform_seek,
                         &IoRingFile::platform_close, buffer,
                         buffer_size, buffer_mode, owned, modeflags,
                         file_descriptor, &IoRingFile::platform_sync),
        handle(h), write_bufs{buffer, nullptr},
        file_buf_size(buffer_size), write_active(0), write_in_flight(false),
        pipeline_region(nullptr), pipeline{}, pipe_head(0), pipe_tail(0),
        pipe_count(0), pipe_pending_sqes(0), pipe_next_offset(0),
        pipe_active(false), pipe_eof(false), write_dirty(false) {}

  // Close NT resources only (drains pipelines, closes handle, frees
  // pipeline/write buffers). Does NOT free pool slot or fd_table entry.
  // Used by freopen for in-place reconstruction.
  int close_nt_resources();
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_FILE_WINDOWS_FILE_H
