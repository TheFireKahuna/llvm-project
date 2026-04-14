//===-- FileOps table: per-kind ops instances --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines the FileOps instances for all FileKind values and kind_to_ops().
//
// Trivial ops (dev_zero, virtual_dir, epoll) are defined inline here.
// Complex I/O ops are defined in their owning TUs and declared in the header.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "hdr/errno_macros.h"
#include "include/llvm-libc-types/struct_iovec.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// DevZero ops -- /dev/zero emulation
// =========================================================================

static ssize_t dev_zero_read(OpenFileDescription *, void *buf, size_t count) {
  __builtin_memset(buf, 0, count);
  return static_cast<ssize_t>(count);
}

static ssize_t dev_zero_write(OpenFileDescription *, const void *,
                              size_t count) {
  return static_cast<ssize_t>(count);
}

static ssize_t dev_zero_readv(OpenFileDescription *, const struct iovec *iov,
                              int iovcnt) {
  size_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > static_cast<size_t>(INT64_MAX) - total)
      return -EINVAL;
    __builtin_memset(iov[i].iov_base, 0, iov[i].iov_len);
    total += iov[i].iov_len;
  }
  return static_cast<ssize_t>(total);
}

static ssize_t dev_zero_writev(OpenFileDescription *, const struct iovec *iov,
                               int iovcnt) {
  size_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > static_cast<size_t>(INT64_MAX) - total)
      return -EINVAL;
    total += iov[i].iov_len;
  }
  return static_cast<ssize_t>(total);
}

const FileOps dev_zero_ops = {
    dev_zero_read, dev_zero_write, dev_zero_readv, dev_zero_writev,
    nullptr,
};

// =========================================================================
// VirtualDir ops
// =========================================================================

static ssize_t vdir_read(OpenFileDescription *, void *, size_t) {
  return -EISDIR;
}
static ssize_t vdir_write(OpenFileDescription *, const void *, size_t) {
  return -EISDIR;
}
static ssize_t vdir_readv(OpenFileDescription *, const struct iovec *, int) {
  return -EISDIR;
}
static ssize_t vdir_writev(OpenFileDescription *, const struct iovec *, int) {
  return -EISDIR;
}

const FileOps virtual_dir_ops = {
    vdir_read, vdir_write, vdir_readv, vdir_writev, nullptr,
};

// =========================================================================
// Epoll ops
// =========================================================================

static ssize_t epoll_err(OpenFileDescription *, void *, size_t) {
  return -EINVAL;
}
static ssize_t epoll_err_w(OpenFileDescription *, const void *, size_t) {
  return -EINVAL;
}
static ssize_t epoll_err_rv(OpenFileDescription *, const struct iovec *, int) {
  return -EINVAL;
}
static ssize_t epoll_err_wv(OpenFileDescription *, const struct iovec *, int) {
  return -EINVAL;
}

const FileOps epoll_fd_ops = {
    epoll_err, epoll_err_w, epoll_err_rv, epoll_err_wv,
    epoll_release_aux,
};

// =========================================================================
// Ops table instances
// =========================================================================
// readv/writev set to nullptr -- the scatter I/O entry points use a generic
// per-iovec loop fallback when the ops entry is null.

const FileOps disk_ops = {
    disk_read_impl, disk_write_impl, nullptr, nullptr,
    disk_release_aux,
};

const FileOps pipe_ops = {
    pipe_read_impl, pipe_write_impl, nullptr, nullptr,
    pipe_release_aux,
};

const FileOps char_ops = {
    char_read_impl, char_write_impl, nullptr, nullptr,
    nullptr,
};

const FileOps condrv_ops = {
    condrv_read_impl, condrv_write_impl, nullptr, nullptr,
    nullptr,
};

const FileOps fifo_ops = {
    fifo_read_impl, fifo_write_impl, nullptr, nullptr,
    fifo_release_aux_impl,
};

const FileOps afd_socket_ops = {
    afd_read_impl, afd_write_impl, nullptr, nullptr,
    afd_release_aux,
};

const FileOps socketpair_ops = {
    sp_read_impl, sp_write_impl, nullptr, nullptr,
    sp_release_aux,
};

const FileOps inotify_ops = {
    inotify_read_impl, epoll_err_w, nullptr, nullptr, // write = EINVAL
    inotify_release_aux,
};

const FileOps pty_master_ops = {
    pty_read_impl, pty_write_impl, nullptr, nullptr,
    pty_master_release_aux,
};

const FileOps pty_slave_ops = {
    pty_read_impl, pty_write_impl, nullptr, nullptr,
    pty_slave_release_aux,
};

// =========================================================================
// kind_to_ops
// =========================================================================

const FileOps *kind_to_ops(FileKind kind) {
  LIBC_ASSERT(kind != FileKind::Auto &&
              "Auto is a sentinel for classify_handle, not a real kind");
  switch (kind) {
  case FileKind::Disk:       return &disk_ops;
  case FileKind::Char:       return &char_ops;
  case FileKind::Pipe:       return &pipe_ops;
  case FileKind::ConDrv:     return &condrv_ops;
  case FileKind::DevZero:    return &dev_zero_ops;
  case FileKind::Inotify:    return &inotify_ops;
  case FileKind::PtyMaster:  return &pty_master_ops;
  case FileKind::PtySlave:   return &pty_slave_ops;
  case FileKind::VirtualDir: return &virtual_dir_ops;
  case FileKind::AfdSocket:  return &afd_socket_ops;
  case FileKind::SocketPair: return &socketpair_ops;
  case FileKind::Epoll:      return &epoll_fd_ops;
  case FileKind::Fifo:       return &fifo_ops;
  case FileKind::Auto:       break;
  }
  __builtin_trap();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
