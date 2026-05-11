//===-- Internal sendfile engine implementation ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Business logic for sendfile().
// Regular file: pread from source, write to destination.
// AFD socket:   IOCTL_AFD_TRANSMIT_FILE -- zero-copy file->socket transfer.
// Socketpair:   NtReadFile from the file, fifo_write to the write channel.
// Returns bytes sent on success, -errno on failure. No libc_errno references.
//
//===----------------------------------------------------------------------===//

#include "sendfile_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/alertable_io.h"
#include "src/__support/OSUtil/windows/io/file_ops.h"
#include "src/__support/OSUtil/windows/io/read_write.h"
#include "src/__support/OSUtil/windows/ipc/afd_core.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/nt_afd.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
  auto *out_ofd = fd_table.get_ofd(out_fd);
  auto *in_ofd = fd_table.get_ofd(in_fd);
  if (!out_ofd || !in_ofd)
    return -EBADF;
  if (out_ofd->is_path_only() || in_ofd->is_path_only())
    return -EBADF;

  // Validate count fits in int64_t before any position pre-claims.
  if (count > static_cast<size_t>(INT64_MAX))
    return -EINVAL;
  int64_t scount = static_cast<int64_t>(count);

  // Determine and validate the file offset.
  int64_t file_offset = 0;
  if (offset) {
    file_offset = *offset;
    if (file_offset < 0)
      return -EINVAL;
    // Validate offset + count won't overflow.
    if (static_cast<uint64_t>(file_offset) >
        static_cast<uint64_t>(INT64_MAX) - count)
      return -EINVAL;
  }

  // Regular file / pipe path: kernel-mode copy via NtCopyFileChunk.
  // Falls back to userspace bounce buffer if NtCopyFileChunk fails
  // (cross-device, unsupported filesystem, etc.), if O_APPEND is set
  // (NtCopyFileChunk uses explicit offsets, can't do atomic append),
  // or if the destination is non-seekable (pipe/FIFO — can't pwrite).
  if (!out_ofd->is_socket()) {
    HANDLE in_h = in_ofd->handle;
    HANDLE out_h = out_ofd->handle;
    bool seekable_out = out_ofd->is_seekable();
    // Note: O_APPEND is loaded once here. A concurrent fcntl(F_SETFL)
    // could change it between this read and the actual write, causing the
    // wrong code path (pwrite vs write-to-end). This is a benign TOCTOU
    // that matches Linux kernel sendfile behavior — O_APPEND races with
    // concurrent fcntl are undefined by POSIX.
    int out_flags = out_ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    bool is_append = seekable_out && (out_flags & O_APPEND);

    // Pre-claim source position range when using fd position.
    // Explicit *offset doesn't touch fd position (adjusted by *offset += total).
    bool src_preclaimed = false;
    if (!offset) {
      file_offset = in_ofd->disk().position.fetch_add(scount,
                                               cpp::MemoryOrder::ACQ_REL);
      src_preclaimed = true;
      if (file_offset < 0 ||
          static_cast<uint64_t>(file_offset) >
              static_cast<uint64_t>(INT64_MAX) - count) {
        in_ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
        return -EINVAL;
      }
    }

    // Pre-claim destination position range (non-append seekable only).
    // O_APPEND uses bounce buffer with internal::write() which handles
    // atomic append via FILE_WRITE_TO_END_OF_FILE.
    // Non-seekable destinations don't use file offsets.
    int64_t dst_offset = 0;
    bool dst_preclaimed = false;
    if (seekable_out && !is_append) {
      dst_offset = out_ofd->disk().position.fetch_add(scount,
                                               cpp::MemoryOrder::ACQ_REL);
      dst_preclaimed = true;
      if (dst_offset < 0 ||
          static_cast<uint64_t>(dst_offset) >
              static_cast<uint64_t>(INT64_MAX) - count) {
        out_ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
        if (src_preclaimed)
          in_ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
        return -EINVAL;
      }
    }

    // Scope guards: give back unused pre-claimed bytes on any exit path.
    // scount is guaranteed valid (validated at function entry).
    size_t total = 0;
    auto src_guard = cpp::make_scope_guard([&] {
      if (offset) {
        *offset += static_cast<off_t>(total);
      } else if (src_preclaimed) {
        int64_t unused = scount - static_cast<int64_t>(total);
        if (unused > 0)
          in_ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
      }
    });
    auto dst_guard = cpp::make_scope_guard([&] {
      if (dst_preclaimed) {
        int64_t unused = scount - static_cast<int64_t>(total);
        if (unused > 0)
          out_ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
      }
      // O_APPEND / non-seekable: write() manages position internally.
    });

    // Try NtCopyFileChunk — kernel-mode file transfer.
    // Skipped for O_APPEND (can't do atomic append with explicit offsets)
    // and non-seekable destinations (NtCopyFileChunk requires explicit offsets).
    windows::ScopedNtHandle evt;
    bool use_kernel_copy = false;
    ssize_t error_result = 0;

    if (seekable_out && !is_append) {
      auto evt_oa = windows::internal_oa();
      NTSTATUS es = ::NtCreateEvent(evt.put(), EVENT_MODIFY_STATE | SYNCHRONIZE,
                                    &evt_oa, SynchronizationEvent, FALSE);
      use_kernel_copy = NT_SUCCESS(es);
    }

    if (use_kernel_copy) {
      while (total < count) {
        ULONG chunk = static_cast<ULONG>(
            (count - total) > 0xFFFFFFFFULL
                ? 0xFFFFFFFFU
                : static_cast<ULONG>(count - total));

        LARGE_INTEGER src_off, dst_off;
        src_off.QuadPart = file_offset + static_cast<int64_t>(total);
        dst_off.QuadPart = dst_offset + static_cast<int64_t>(total);

        IO_STATUS_BLOCK iosb = {};
        NTSTATUS s = ::NtCopyFileChunk(in_h, out_h, evt.get(), &iosb, chunk,
                                       &src_off, &dst_off,
                                       nullptr, nullptr, 0);
        if (s == STATUS_PENDING) {
          s = alertable_wait_restartable(evt.get());
          if (s == STATUS_USER_APC) {
            if (total == 0)
              error_result = -EINTR;
            break;
          }
          if (NT_SUCCESS(s))
            s = iosb.Status;
        }

        if (!NT_SUCCESS(s)) {
          if (total == 0 && s != STATUS_END_OF_FILE) {
            // First chunk failed — fall back to bounce buffer.
            use_kernel_copy = false;
            break;
          }
          break; // Partial or EOF — return what we have.
        }

        size_t copied = static_cast<size_t>(iosb.Information);
        if (copied == 0)
          break; // EOF
        total += copied;
        if (copied < chunk)
          break; // Short copy — EOF reached.
      }
    }

    // Fallback: userspace bounce buffer (cross-device, unsupported FS,
    // or O_APPEND destinations).
    // NOT entered on EINTR — that's a legitimate interruption, not a
    // transport failure.
    if (!use_kernel_copy && error_result == 0) {
      // The originally pre-claimed destination range (if any) is still valid —
      // no bytes were written by the failed kernel-copy attempt. Use pwrite
      // at those offsets for non-append, write() for O_APPEND.

      static constexpr size_t BOUNCE_SIZE = 65536;
      auto *buf = static_cast<uint8_t *>(internal::page_alloc(BOUNCE_SIZE));
      if (!buf)
        return -ENOMEM;
      auto free_buf = cpp::make_scope_guard([&] { internal::page_free(buf); });

      bool bounce_abort = false;
      while (total < count && !bounce_abort) {
        size_t chunk = count - total;
        if (chunk > BOUNCE_SIZE)
          chunk = BOUNCE_SIZE;

        off_t src_pos =
            static_cast<off_t>(file_offset) + static_cast<off_t>(total);
        intptr_t r = internal::pread(in_fd, buf, chunk, src_pos);
        if (r == 0)
          break;
        if (r < 0) {
          if (total == 0)
            error_result = static_cast<ssize_t>(r);
          break;
        }

        size_t got = static_cast<size_t>(r);
        size_t buf_off = 0;
        while (buf_off < got) {
          intptr_t w;
          if (!seekable_out || is_append) {
            // Non-seekable (pipe/FIFO) or O_APPEND: write() handles both.
            // O_APPEND uses FILE_WRITE_TO_END_OF_FILE for atomic append.
            // Non-seekable uses kernel-default sequential write.
            w = internal::write(out_fd, buf + buf_off, got - buf_off);
          } else {
            // Seekable non-append: pwrite at explicit offset from
            // pre-claimed range.
            off_t dst_pos = static_cast<off_t>(dst_offset) +
                            static_cast<off_t>(total) +
                            static_cast<off_t>(buf_off);
            w = internal::pwrite(out_fd, buf + buf_off, got - buf_off,
                                 dst_pos);
          }
          if (w <= 0) {
            if (total == 0)
              error_result = (w < 0 ? static_cast<ssize_t>(w)
                                    : static_cast<ssize_t>(-EIO));
            bounce_abort = true;
            break;
          }
          buf_off += static_cast<size_t>(w);
          total += static_cast<size_t>(w);
        }
      }
    }

    // Scope guards handle all position/offset updates on return.
    if (total > 0)
      return static_cast<ssize_t>(total);
    return static_cast<ssize_t>(error_result);
  }

  HANDLE out_h = out_ofd->handle;
  HANDLE in_h = in_ofd->handle;

  // Socketpair path: userspace read+write loop.
  if (out_ofd->kind == FileKind::SocketPair) {
    // Pre-claim source position when using fd position.
    if (!offset) {
      file_offset = in_ofd->disk().position.fetch_add(scount,
                                               cpp::MemoryOrder::ACQ_REL);
      if (file_offset < 0 ||
          static_cast<uint64_t>(file_offset) >
              static_cast<uint64_t>(INT64_MAX) - count) {
        in_ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
        return -EINVAL;
      }
    }

    size_t total = 0;

    // Scope guard: give back unused source bytes and update offset on exit.
    auto sp_src_guard = cpp::make_scope_guard([&] {
      if (offset) {
        *offset += static_cast<off_t>(total);
      } else {
        int64_t unused = scount - static_cast<int64_t>(total);
        if (unused > 0)
          in_ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
      }
    });

    auto *sp = out_ofd->socket_pair();
    static constexpr size_t SP_BOUNCE_SIZE = 65536;
    auto *buf = static_cast<uint8_t *>(internal::page_alloc(SP_BOUNCE_SIZE));
    if (!buf)
      return -ENOMEM;
    auto free_buf = cpp::make_scope_guard([&] { internal::page_free(buf); });

    ssize_t last_err = -EIO;

    // Create event once outside the loop — avoid 256 create/destroy cycles
    // for a 1MB file.
    windows::ScopedNtHandle evt;
    auto evt_oa = windows::internal_oa();
    NTSTATUS es = NtCreateEvent(evt.put(), EVENT_MODIFY_STATE | SYNCHRONIZE,
                                &evt_oa, SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(es))
      return -windows_util::ntstatus_to_errno(es);

    bool sp_abort = false;
    while (total < count && !sp_abort) {
      size_t chunk = count - total;
      if (chunk > SP_BOUNCE_SIZE)
        chunk = SP_BOUNCE_SIZE;

      // Read from the file at the specified offset.
      LARGE_INTEGER li;
      li.QuadPart = file_offset + static_cast<int64_t>(total);
      IO_STATUS_BLOCK iosb = {};

      NTSTATUS s = NtReadFile(in_h, evt.get(), nullptr, nullptr, &iosb, buf,
                              static_cast<ULONG>(chunk), &li, nullptr);
      if (s == STATUS_PENDING) {
        s = alertable_wait_restartable(evt.get());
        if (s == STATUS_USER_APC) {
          if (total == 0)
            last_err = -EINTR;
          break; // sp_abort path — nothing more to do.
        }
        if (NT_SUCCESS(s))
          s = iosb.Status;
      }

      if (!NT_SUCCESS(s)) {
        if (total == 0)
          last_err = -windows_util::ntstatus_to_errno(s);
        break;
      }
      if (iosb.Information == 0)
        break; // EOF.

      size_t got = static_cast<size_t>(iosb.Information);
      int open_flags = out_ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);

      // Write all read bytes — retry on partial/EINTR to avoid data loss.
      size_t buf_off = 0;
      while (buf_off < got) {
        ssize_t w = fifo_write(sp->write_ch, buf + buf_off, got - buf_off,
                               open_flags);
        if (w == -EINTR) {
          if (signal_state::should_restart_syscall())
            continue; // SA_RESTART — retry the write.
          if (total == 0 && buf_off == 0)
            last_err = -EINTR;
          sp_abort = true;
          break;
        }
        if (w <= 0) {
          last_err = (w == 0) ? -EPIPE : w;
          sp_abort = true;
          break;
        }
        buf_off += static_cast<size_t>(w);
        total += static_cast<size_t>(w);
      }
    }

    // evt closed by ScopedNtHandle destructor.
    // Scope guard handles source position/offset update on return.
    if (total == 0 && count > 0)
      return last_err;
    return static_cast<ssize_t>(total);
  }

  // AFD socket path: IOCTL_AFD_TRANSMIT_FILE -- zero-copy.
  auto *state = out_ofd->afd_socket();
  long connect_state = internal::socket_reap_connect_if_needed(state);
  if (connect_state < 0)
    return connect_state;

  // Check if the socket was shut down for writing.
  uint8_t shut = state->shutdown_flags.load(cpp::MemoryOrder::ACQUIRE);
  if (shut & 2) // SHUT_WR
    return -EPIPE;

  // Pre-claim source position when using fd position.
  if (!offset) {
    file_offset = in_ofd->disk().position.fetch_add(scount,
                                                    cpp::MemoryOrder::ACQ_REL);
    if (file_offset < 0 ||
        static_cast<uint64_t>(file_offset) >
            static_cast<uint64_t>(INT64_MAX) - count) {
      in_ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
      return -EINVAL;
    }
  }

  AFD_TRANSMIT_FILE_INFO tfi = {};
  tfi.Offset.QuadPart = file_offset;
  tfi.WriteLength.QuadPart = static_cast<LONGLONG>(count);
  tfi.SendPacketLength = 0; // Default packet size.
  tfi.FileHandle = in_h;
  tfi.Head = nullptr;
  tfi.HeadLength = 0;
  tfi.Tail = nullptr;
  tfi.TailLength = 0;
  tfi.Flags = 0;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s;
  for (;;) {
    iosb = {};
    s = afd_ioctl(out_h, IOCTL_AFD_TRANSMIT_FILE, &tfi, sizeof(tfi), nullptr, 0,
                  &iosb);
    if (s == STATUS_CANCELLED &&
        signal_state::should_restart_syscall())
      continue; // SA_RESTART — retry the transmit.
    break;
  }
  if (!NT_SUCCESS(s)) {
    // Give back all pre-claimed source position bytes.
    if (!offset)
      in_ofd->disk().position.fetch_sub(scount, cpp::MemoryOrder::ACQ_REL);
    return -ntstatus_to_errno_socket(s);
  }

  ssize_t sent = static_cast<ssize_t>(iosb.Information);
  if (offset) {
    *offset += sent;
  } else {
    // Give back unused pre-claimed bytes.
    int64_t unused = scount - static_cast<int64_t>(sent);
    if (unused > 0)
      in_ofd->disk().position.fetch_sub(unused, cpp::MemoryOrder::ACQ_REL);
  }
  return sent;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
