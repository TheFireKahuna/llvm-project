//===-- Windows internal chmod operations ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for chmod/fchmod/fchmodat on Windows. These
// implement Linux syscall semantics: 0 on success, -errno on failure.
// Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "chmod_ops.h"
#include "hdr/errno_macros.h"
#include "src/__support/error_or.h"
#include "src/__support/CPP/string_view.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t chmod(const char *path, mode_t mode) {
  using LIBC_NAMESPACE::cpp::string_view;
  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  string_view sv(path);
  auto nt = to_nt_path(sv, path_buf, path_buf_s.size());
  if (!nt.has_value())
    return -nt.error();
  size_t path_len = nt.value();

  windows::nt_wstring_view name(path_buf, path_len);
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &name);

  // The owner ACE always includes WRITE_DAC | FILE_READ_ATTRIBUTES |
  // FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA (see build_posix_dacl), so
  // requesting all of them succeeds for any file the caller owns.
  // WRITE_DAC is also an implicit owner right on NTFS.
  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(),
      WRITE_DAC | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA,
      &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_OPEN_FOR_BACKUP_INTENT);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  int err = windows_sec::chmod_impl(handle.get(), mode);

  if (err)
    return -err;
  return 0;
}

intptr_t fchmod(int fd, mode_t mode) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  HANDLE h = ofd->handle;
  if (!h)
    return -EBADF;

  int err = windows_sec::chmod_impl(h, mode);
  if (err)
    return -err;
  return 0;
}

intptr_t fchmodat(int dfd, const char *path, mode_t mode, int flags) {
  using LIBC_NAMESPACE::cpp::string_view;
  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  windows::nt_wstring_view name;
  OBJECT_ATTRIBUTES oa;

  string_view sv(path);
  int err = resolve_at_path(dfd, sv, path_buf, path_buf_s.size(), &oa, &name,
                            flags);
  if (err)
    return -err;

  // FILE_OPEN_REPARSE_POINT opens the symlink itself (sets its DACL)
  // rather than following it. Unlike OBJ_DONT_REPARSE (which fails with
  // STATUS_STOPPED_ON_SYMLINK), this gives us a handle to the reparse
  // point — NT supports chmod on symlinks, unlike Linux.
  ULONG open_opts = FILE_OPEN_FOR_BACKUP_INTENT;
  if (flags & AT_SYMLINK_NOFOLLOW)
    open_opts |= FILE_OPEN_REPARSE_POINT;

  // Same access mask as chmod — see comment there.
  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(),
      WRITE_DAC | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA,
      &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      open_opts);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  err = windows_sec::chmod_impl(handle.get(), mode);

  if (err)
    return -err;
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
