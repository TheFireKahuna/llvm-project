//===-- Windows internal stat operations -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for stat/lstat/fstat/fstatat on Windows. These
// implement Linux syscall semantics: 0 on success, -errno on failure.
// Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "stat_ops.h"
#include "nt_to_stat.h"
#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/path_resolver.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Forward-declare fstat for use by stat_virtual_path (/dev/fd/<n> delegation).
intptr_t fstat(int fd, struct stat *statbuf);

// Synthesize a struct stat for POSIX /dev/* virtual device and pseudo paths.
// NT device objects (e.g. \Device\Null) are not filesystem objects, so
// NtQueryInformationByName(FileStatBasicInformation) doesn't apply.
//
// Returns: 0 = handled with stat filled, positive = not handled (fall through),
// negative = -errno error.
// Uses classify_path() / resolve_dev_fd() from path_resolver.h.
static int stat_virtual_path(const char *path, struct stat *statbuf) {
  using LIBC_NAMESPACE::cpp::string_view;

  PathKind kind = classify_path(path);
  if (path) {
    const string_view sv(path);
    if (sv.starts_with("/dev") && (sv.size() == 4 || sv[4] == '/')) {
      const DevSubpathInfo dev = classify_dev_subpath_info(sv);
      if (dev.error)
        return -dev.error;
      kind = dev.kind;
    }
  }

  // /dev/fd/<n>, /dev/stdin, /dev/stdout, /dev/stderr: delegate to fstat.
  if (kind == PathKind::DevFd) {
    int fd = resolve_dev_fd(path);
    if (fd < 0)
      return -ENOENT;
    intptr_t r = fstat(fd, statbuf);
    return (r < 0) ? static_cast<int>(r) : 0;
  }

  // /dev/pts/*, /dev/ptmx, /dev, /dev/pts: synthetic S_IFCHR stat.
  if (kind == PathKind::DevPty) {
    __builtin_memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                        S_IROTH | S_IWOTH; // crw-rw-rw- (0666)
    statbuf->st_nlink = 1;
    statbuf->st_blksize = 4096;
    statbuf->st_rdev = static_cast<dev_t>((136 << 8) | 0); // 136:N (Linux pts)
    return 0;
  }

  switch (kind) {
  case PathKind::DevNull:
  case PathKind::DevZero:
  case PathKind::DevRandom:
  case PathKind::DevUrandom:
  case PathKind::DevTty:
    break; // Recognized — fill stat below.
  default:
    return 1; // Not a virtual path — caller should fall through.
  }

  __builtin_memset(statbuf, 0, sizeof(*statbuf));
  statbuf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                      S_IROTH | S_IWOTH; // crw-rw-rw- (0666)
  statbuf->st_nlink = 1;
  statbuf->st_blksize = 4096;
  // st_rdev encodes the device identity. Use well-known Linux major:minor
  // numbers for compatibility with test suites (major 1, minor 3/5/8/9).
  switch (kind) {
  case PathKind::DevNull:
    statbuf->st_rdev = static_cast<dev_t>((1 << 8) | 3);   // 1:3
    break;
  case PathKind::DevZero:
    statbuf->st_rdev = static_cast<dev_t>((1 << 8) | 5);   // 1:5
    break;
  case PathKind::DevRandom:
    statbuf->st_rdev = static_cast<dev_t>((1 << 8) | 8);   // 1:8
    break;
  case PathKind::DevUrandom:
    statbuf->st_rdev = static_cast<dev_t>((1 << 8) | 9);   // 1:9
    break;
  case PathKind::DevTty:
    statbuf->st_rdev = static_cast<dev_t>((5 << 8) | 0);   // 5:0
    break;
  default:
    break;
  }
  return 0;
}

intptr_t stat(const char *__restrict path, struct stat *__restrict statbuf) {
  // /dev/* virtual device paths need synthetic stat — they are NT device
  // objects, not filesystem objects queryable via NtQueryInformationByName.
  {
    int vr = stat_virtual_path(path, statbuf);
    if (vr <= 0)
      return static_cast<intptr_t>(vr); // 0 = success, negative = -errno.
  }

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  size_t path_len = to_nt_path(path, path_s.data(), path_s.size());
  if (path_len == 0)
    return -EINVAL;

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, path_s.data(), path_len);

  // Always query FileStatBasicInformation — provides st_dev
  // (VolumeSerialNumber) and all base fields in one handleless syscall.
  IO_STATUS_BLOCK iosb = {};
  FILE_STAT_BASIC_INFORMATION info;
  NTSTATUS status = NtQueryInformationByName(
      &oa, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      FileStatBasicInformation);

  if (!NT_SUCCESS(status)) {
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND)
      return -ENOENT;
    else if (status == STATUS_ACCESS_DENIED)
      return -EACCES;
    else
      return -EIO;
  }

  nt_stat_basic_to_stat(info, statbuf);

  // Enrich mode/uid/gid from FileStatLxInformation if available.
  FILE_STAT_LX_INFORMATION lx_info;
  status = NtQueryInformationByName(
      &oa, &iosb, &lx_info, static_cast<ULONG>(sizeof(lx_info)),
      FileStatLxInformation);
  if (NT_SUCCESS(status))
    nt_stat_lx_enrich(lx_info, statbuf);

  // Fallback: if uid/gid still 0 (no WSL EAs), use euid/egid.
  stat_fill_default_uid_gid(statbuf);

  fixup_fifo_mode(&oa, info, statbuf);
  return 0;
}

intptr_t lstat(const char *__restrict path, struct stat *__restrict statbuf) {
  // /dev/* virtual device paths are never symlinks; handle identically to stat.
  {
    int vr = stat_virtual_path(path, statbuf);
    if (vr <= 0)
      return static_cast<intptr_t>(vr); // 0 = success, negative = -errno.
  }

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  size_t path_len = to_nt_path(path, path_s.data(), path_s.size());
  if (path_len == 0)
    return -EINVAL;

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, path_s.data(), path_len);
  // Don't follow reparse points (symlinks).
  oa.Attributes |= OBJ_DONT_REPARSE;

  IO_STATUS_BLOCK iosb = {};
  FILE_STAT_BASIC_INFORMATION info;
  NTSTATUS status = NtQueryInformationByName(
      &oa, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      FileStatBasicInformation);

  if (!NT_SUCCESS(status)) {
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND)
      return -ENOENT;
    else if (status == STATUS_ACCESS_DENIED)
      return -EACCES;
    else
      return -EIO;
  }

  nt_stat_basic_to_stat(info, statbuf);

  // Enrich mode/uid/gid from LxInfo if available.
  FILE_STAT_LX_INFORMATION lx_info;
  status = NtQueryInformationByName(
      &oa, &iosb, &lx_info, static_cast<ULONG>(sizeof(lx_info)),
      FileStatLxInformation);
  if (NT_SUCCESS(status))
    nt_stat_lx_enrich(lx_info, statbuf);

  // Fallback: if uid/gid still 0 (no WSL EAs), use euid/egid.
  stat_fill_default_uid_gid(statbuf);

  fixup_fifo_mode(&oa, info, statbuf);
  return 0;
}

intptr_t fstat(int fd, struct stat *statbuf) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;

  // FIFO fds use a shared-memory ring buffer — no real handle to query.
  if (ofd->is_fifo()) {
    __builtin_memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_mode = S_IFIFO | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                        S_IROTH | S_IWOTH; // 0666
    statbuf->st_nlink = 1;
    statbuf->st_blksize = 4096;
    return 0;
  }

  HANDLE h = ofd->handle;
  if (!h)
    return -EBADF;

  IO_STATUS_BLOCK iosb = {};
  FILE_STAT_BASIC_INFORMATION info;
  NTSTATUS status = NtQueryInformationFile(
      h, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      FileStatBasicInformation);

  if (!NT_SUCCESS(status))
    return -EIO;

  nt_stat_basic_to_stat(info, statbuf);

  // Enrich st_mode via three-tier reconstruction (EA → DACL → attribute).
  // Falls back gracefully if handle lacks READ_CONTROL or FILE_READ_EA.
  statbuf->st_mode = stat_mode_from_handle(h, statbuf->st_mode);

  // Enrich uid/gid from $LXUID/$LXGID EAs. Falls back to euid/egid when
  // no EA metadata exists (e.g. files created by non-NTPOSIX tools).
  stat_uid_gid_from_handle(h, statbuf);

  // Fill st_dev from the volume serial number (not available from
  // FILE_STAT_BASIC_INFORMATION on all code paths).
  statbuf->st_dev = query_volume_serial(h);

  return 0;
}

intptr_t fstatat(int dfd, const char *__restrict path,
             struct stat *__restrict statbuf, int flags) {
  // AT_EMPTY_PATH: stat the fd itself, not a path relative to it.
  // Delegate to fstat which uses NtQueryInformationFile on the handle.
  if ((flags & AT_EMPTY_PATH) && path[0] == '\0')
    return fstat(dfd, statbuf);

  // /dev/* virtual device paths: synthetic stat regardless of dirfd.
  {
    int vr = stat_virtual_path(path, statbuf);
    if (vr <= 0)
      return static_cast<intptr_t>(vr); // 0 = success, negative = -errno.
  }

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;

  int err = resolve_at_path(dfd, path, path_s.data(), path_s.size(), &oa, &us);
  if (err)
    return -err;

  if (flags & AT_SYMLINK_NOFOLLOW)
    oa.Attributes |= OBJ_DONT_REPARSE;

  IO_STATUS_BLOCK iosb = {};
  FILE_STAT_BASIC_INFORMATION info;
  NTSTATUS status = NtQueryInformationByName(
      &oa, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      FileStatBasicInformation);

  if (!NT_SUCCESS(status)) {
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND)
      return -ENOENT;
    else if (status == STATUS_ACCESS_DENIED)
      return -EACCES;
    else
      return -windows_util::ntstatus_to_errno(status);
  }

  nt_stat_basic_to_stat(info, statbuf);

  FILE_STAT_LX_INFORMATION lx_info;
  status = NtQueryInformationByName(
      &oa, &iosb, &lx_info, static_cast<ULONG>(sizeof(lx_info)),
      FileStatLxInformation);
  if (NT_SUCCESS(status))
    nt_stat_lx_enrich(lx_info, statbuf);

  // Fallback: if uid/gid still 0 (no WSL EAs), use euid/egid.
  stat_fill_default_uid_gid(statbuf);

  fixup_fifo_mode(&oa, info, statbuf);
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
