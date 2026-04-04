//===-- Internal ownership operations for Windows ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine functions for chown, fchown, fchownat.
// Each returns 0 on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#include "ownership_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/error_or.h"
#include "src/__support/CPP/string_view.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t chown(const char *path, uid_t owner, gid_t group) {
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

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), WRITE_DAC | FILE_WRITE_EA,
      &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  int err = windows_sec::chown_impl(handle.get(), owner, group);

  if (err)
    return -err;
  return 0;
}

intptr_t fchown(int fd, uid_t owner, gid_t group) {
  internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  HANDLE h = ofd->handle;
  if (!h)
    return -EBADF;

  int err = windows_sec::chown_impl(h, owner, group);
  if (err)
    return -err;
  return 0;
}

intptr_t fchownat(int dfd, const char *path, uid_t owner, gid_t group,
              int flags) {
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

  ULONG open_opts = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT;
  if (flags & AT_SYMLINK_NOFOLLOW)
    open_opts |= FILE_OPEN_REPARSE_POINT;

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), WRITE_DAC | FILE_WRITE_EA,
      &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      open_opts);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  err = windows_sec::chown_impl(handle.get(), owner, group);

  if (err)
    return -err;
  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
