//===-- FileOps table: per-kind ops instances + section walker --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines the FileOps instances for always-pulled FileKinds (disk, pipe,
// char, condrv, dev_zero, virtual_dir) and hosts the .libcops registry
// bookends + walker.
//
// Optional FileKinds (epoll, inotify, pty_master, pty_slave, afd_socket,
// socketpair, fifo) register their FileOps instances into .libcops$M
// from their owning TUs via LIBC_REGISTER_FILE_OPS. If the owning TU
// isn't pulled into the link, the kind's slot stays absent and
// kind_to_ops() traps on null.
//
// init_file_ops_table() (called from fd_table_startup_init during
// Tier B) walks the section range once, populates a flat dispatch
// array keyed by FileKind, and verifies the output section is
// MEM_READ without MEM_WRITE -- a belt-and-braces check against a
// downstream `#pragma section(".libcops$M", read, write)` unioning in
// MEM_WRITE and silently defeating the read-only posture.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_ops_section.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
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
// Always-pulled I/O kinds
// readv/writev set to nullptr -- the scatter I/O entry points use a generic
// per-iovec loop fallback when the ops entry is null.
// =========================================================================

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

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// =========================================================================
// .libcops registry bookends
//
// Emits libc_libcops_section_start / _end into .libcops$A / $Z and
// materializes libc_libcops_registry() returning a typed view over the
// merged range. Optional-kind registrations from other TUs land in
// .libcops$M between them.
// =========================================================================

LIBC_DEFINE_SECTION_BOOKENDS(libcops,
                             ::LIBC_NAMESPACE::internal::FileOpsRegistration)

// Force-pull every TU that calls LIBC_REGISTER_FILE_OPS. Each such call
// emits a `__libc_ops_anchor_<kind>` extern. Without this the .obj's
// only outward artifact is the .libcops$M section record, which COFF
// archive selection ignores — optional kinds (epoll, inotify, etc.)
// would silently miss their dispatch slot in per-test trimmed libc.lib.
LIBC_FORCE_PULL_GLOB("__libc_ops_anchor_*");

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Kind → FileOps* dispatch table
//
// Populated at Tier B init from (a) hard-coded always-pulled kinds and
// (b) section-registered optional kinds. kind_to_ops() is a flat array
// lookup with no switch, no indirect call.
// =========================================================================

static constexpr size_t kKindCount = static_cast<size_t>(FileKind::Fifo) + 1;
static const FileOps *g_kind_to_ops_table[kKindCount] = {};

void init_file_ops_table() {
  // Always-pulled kinds — their ops TUs are this file, which is part of
  // the fd_table closure.
  g_kind_to_ops_table[static_cast<size_t>(FileKind::Disk)] = &disk_ops;
  g_kind_to_ops_table[static_cast<size_t>(FileKind::Char)] = &char_ops;
  g_kind_to_ops_table[static_cast<size_t>(FileKind::Pipe)] = &pipe_ops;
  g_kind_to_ops_table[static_cast<size_t>(FileKind::ConDrv)] = &condrv_ops;
  g_kind_to_ops_table[static_cast<size_t>(FileKind::DevZero)] = &dev_zero_ops;
  g_kind_to_ops_table[static_cast<size_t>(FileKind::VirtualDir)] =
      &virtual_dir_ops;

  // Optional kinds — registered via .libcops$M by their owning TUs.
  auto registry = libc_libcops_registry();

  // PE-characteristics audit: confirm the merged .libcops output section
  // has MEM_READ set and MEM_WRITE clear. A failure here means a
  // downstream TU unioned MEM_WRITE into the section via a
  // `#pragma section(..., read, write)` directive, silently defeating
  // the read-only posture. Release-mode hard fail — every fd op vector
  // (read/write/close/...) is dispatched through this table on every
  // syscall, so a writable .libcops is direct CFG bypass.
  enforce_section_readonly_or_fastfail(g_pcb.zone0.module_handle(),
                                       registry.probe_address(), ".libcops");

  for (const auto &rec : registry) {
    if (rec.ops == nullptr || rec.kind == FileKind::Auto)
      continue;
    size_t idx = static_cast<size_t>(rec.kind);
    LIBC_ASSERT(idx < kKindCount && "FileOpsRegistration index out of range");
    g_kind_to_ops_table[idx] = rec.ops;
  }
}

const FileOps *kind_to_ops(FileKind kind) {
  LIBC_ASSERT(kind != FileKind::Auto &&
              "Auto is a sentinel for classify_handle, not a real kind");
  size_t idx = static_cast<size_t>(kind);
  LIBC_ASSERT(idx < kKindCount && "FileKind out of range");
  const FileOps *ops = g_kind_to_ops_table[idx];
  LIBC_ASSERT(ops != nullptr &&
              "FileKind ops not registered — owning TU wasn't linked");
  return ops;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
