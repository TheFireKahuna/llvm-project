//===-- Per-kind file operations table ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux VFS-style operations table for OpenFileDescription. Each FileKind
// has a const FileOps instance with function pointers for read, write,
// readv, writev, and release. Dispatch is a single indirect call:
//
//   ofd->ops->read(ofd, buf, count)
//
// The ops tables are const with static storage duration and live in .rdata --
// immutable, always in cache, branch-predictor-friendly (stable indirect call
// target per-fd).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FILE_OPS_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FILE_OPS_TABLE_H

#include "include/llvm-libc-types/ssize_t.h"
#include "src/__support/macros/config.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations to avoid pulling in heavy headers.
struct iovec;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription;
enum class FileKind : uint8_t;

// ---------------------------------------------------------------------------
// FileOps -- per-kind operations table
// ---------------------------------------------------------------------------

struct FileOps {
  // Scalar read/write. Returns bytes transferred (>= 0) or -errno.
  ssize_t (*read)(OpenFileDescription *ofd, void *buf, size_t count);
  ssize_t (*write)(OpenFileDescription *ofd, const void *buf, size_t count);

  // Vectored read/write. Returns bytes transferred (>= 0) or -errno.
  // nullptr is valid: scatter_io_ops.cpp falls back to a per-iovec loop
  // calling scalar read/write when the vectored entry is null. Most kinds
  // leave these null and rely on the fallback; only kinds that need
  // specialized scatter behavior (e.g., dev_zero, virtual_dir) populate them.
  ssize_t (*readv)(OpenFileDescription *ofd, const struct iovec *iov,
                   int iovcnt);
  ssize_t (*writev)(OpenFileDescription *ofd, const struct iovec *iov,
                    int iovcnt);

  // Kind-specific resource cleanup. Called by OFD::release() before NtClose.
  // Returns true if the caller should NtClose the handle, false if the
  // release_aux implementation already owns the handle lifetime (e.g., PTY
  // master where the session owns the handle). nullptr means no kind-specific
  // cleanup needed and the handle should be closed normally.
  bool (*release_aux)(OpenFileDescription *ofd);
};

// ---------------------------------------------------------------------------
// Per-kind impl function declarations
// ---------------------------------------------------------------------------
// Defined in their owning TUs (read_write.cpp, epoll_ops.cpp, etc.).
// Declared here so that file_ops_table.cpp and the definition sites share
// a single declaration -- preventing silent ODR violations from signature
// mismatches.

// read_write.cpp -- IoRing-based kinds
ssize_t disk_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t disk_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count);
ssize_t pipe_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t pipe_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count);
ssize_t char_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t char_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count);
ssize_t afd_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t afd_write_impl(OpenFileDescription *ofd, const void *buf,
                        size_t count);

// read_write.cpp -- release_aux
bool disk_release_aux(OpenFileDescription *ofd);
bool pipe_release_aux(OpenFileDescription *ofd);
bool afd_release_aux(OpenFileDescription *ofd);

// read_write.cpp -- fifo/socketpair (ring buffer, not IoRing)
ssize_t fifo_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t fifo_write_impl(OpenFileDescription *ofd, const void *buf,
                         size_t count);
ssize_t sp_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t sp_write_impl(OpenFileDescription *ofd, const void *buf,
                       size_t count);
bool fifo_release_aux_impl(OpenFileDescription *ofd);
bool sp_release_aux(OpenFileDescription *ofd);

// read_write.cpp -- console/pty delegates
ssize_t condrv_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t condrv_write_impl(OpenFileDescription *ofd, const void *buf,
                           size_t count);
ssize_t pty_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
ssize_t pty_write_impl(OpenFileDescription *ofd, const void *buf,
                        size_t count);
bool pty_master_release_aux(OpenFileDescription *ofd);
bool pty_slave_release_aux(OpenFileDescription *ofd);

// read_write.cpp -- inotify
ssize_t inotify_read_impl(OpenFileDescription *ofd, void *buf, size_t count);
bool inotify_release_aux(OpenFileDescription *ofd);

// epoll_ops.cpp
bool epoll_release_aux(OpenFileDescription *ofd);

// ---------------------------------------------------------------------------
// Per-kind ops table declarations (defined in file_ops_table.cpp)
// ---------------------------------------------------------------------------

extern const FileOps disk_ops;
extern const FileOps char_ops;
extern const FileOps pipe_ops;
extern const FileOps condrv_ops;
extern const FileOps dev_zero_ops;
extern const FileOps inotify_ops;
extern const FileOps pty_master_ops;
extern const FileOps pty_slave_ops;
extern const FileOps virtual_dir_ops;
extern const FileOps afd_socket_ops;
extern const FileOps socketpair_ops;
extern const FileOps epoll_fd_ops;
extern const FileOps fifo_ops;

// Map a FileKind to its ops table. Used by alloc() when the caller doesn't
// supply an explicit ops pointer.
const FileOps *kind_to_ops(FileKind kind);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FILE_OPS_TABLE_H
