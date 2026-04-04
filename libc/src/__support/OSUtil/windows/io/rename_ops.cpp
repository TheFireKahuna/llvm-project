//===-- Windows internal rename/remove operations --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for file removal and renaming on Windows. These
// implement Linux syscall semantics in userspace: 0 on success, -errno on
// failure. Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "rename_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/string_view.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/error_or.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t remove_file(const char *path) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  string_view sv(path);

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  auto nt = to_nt_path(sv, path_s.data(), path_s.size());
  if (!nt.has_value())
    return -nt.error();
  size_t path_len = nt.value();

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name(path_s.data(), path_len);
  init_object_attributes(&oa, &name);

  // Try file deletion first (single syscall, no handle needed).
  NTSTATUS status = ::NtDeleteFile(&oa);
  if (NT_SUCCESS(status))
    return 0;

  // NtDeleteFile fails on directories — open and delete via disposition.
  if (status == STATUS_FILE_IS_A_DIRECTORY) {
    windows::ScopedNtHandle handle;
    IO_STATUS_BLOCK iosb = {};
    status = ::NtOpenFile(handle.put(), DELETE_ACCESS | SYNCHRONIZE, &oa, &iosb,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                              FILE_OPEN_FOR_BACKUP_INTENT);
    if (NT_SUCCESS(status)) {
      FILE_DISPOSITION_INFORMATION_EX disp = {};
      disp.Flags = FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS;
      status = ::NtSetInformationFile(handle.get(), &iosb, &disp,
                                      static_cast<ULONG>(sizeof(disp)),
                                      FileDispositionInformationEx);
    }
  }

  if (NT_SUCCESS(status))
    return 0;

  return -windows_util::ntstatus_to_errno(status);
}

intptr_t rename_file(const char *oldpath, const char *newpath) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!oldpath || !newpath)
    return -EFAULT;
  string_view old_sv(oldpath);
  string_view new_sv(newpath);

  // Open the source file/directory for rename.
  auto old_s = path_scratch();
  if (!old_s) return -ENOMEM;
  WCHAR *old_buf = old_s.data();
  auto nt_old = to_nt_path(old_sv, old_buf, old_s.size());
  if (!nt_old.has_value())
    return -nt_old.error();
  size_t old_len = nt_old.value();

  OBJECT_ATTRIBUTES old_oa;
  windows::nt_wstring_view old_name(old_buf, old_len);
  init_object_attributes(&old_oa, &old_name);

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), DELETE_ACCESS | SYNCHRONIZE, &old_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Convert destination path to NT format.
  auto new_s = path_scratch();
  if (!new_s)
    return -ENOMEM;
  WCHAR *new_buf = new_s.data();
  auto nt_new = to_nt_path(new_sv, new_buf, new_s.size());
  if (!nt_new.has_value())
    return -nt_new.error();
  size_t new_len = nt_new.value();

  // Build FILE_RENAME_INFORMATION with the destination path inline.
  // Use FileRenameInformationEx for POSIX semantics (atomic replace).
  ULONG name_bytes = static_cast<ULONG>(new_len * sizeof(WCHAR));
  ULONG info_size =
      static_cast<ULONG>(offsetof(FILE_RENAME_INFORMATION, FileName)) +
      name_bytes;

  auto info_s = info_scratch<FILE_RENAME_INFORMATION>();
  if (!info_s)
    return -ENOMEM;
  auto *info = reinterpret_cast<FILE_RENAME_INFORMATION *>(info_s.data());

  info->Flags = FILE_RENAME_REPLACE_IF_EXISTS | FILE_RENAME_POSIX_SEMANTICS;
  info->RootDirectory = nullptr;
  info->FileNameLength = name_bytes;
  __builtin_memcpy(info->FileName, new_buf, name_bytes);

  status = ::NtSetInformationFile(handle.get(), &iosb, info, info_size,
                                  FileRenameInformationEx);

  if (NT_SUCCESS(status))
    return 0;

  return -windows_util::ntstatus_to_errno(status);
}

intptr_t renameat(int olddfd, const char *oldpath,
              int newdfd, const char *newpath) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!oldpath || !newpath)
    return -EFAULT;
  string_view old_sv(oldpath);
  string_view new_sv(newpath);

  // Resolve source path.
  auto old_s2 = path_scratch();
  if (!old_s2) return -ENOMEM;
  OBJECT_ATTRIBUTES old_oa;
  windows::nt_wstring_view old_name;

  int err = resolve_at_path(olddfd, old_sv, old_s2.data(), old_s2.size(),
                            &old_oa, &old_name);
  if (err)
    return -err;

  // Open source for rename. FILE_OPEN_REPARSE_POINT renames the symlink
  // itself, not its target.
  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), DELETE_ACCESS | SYNCHRONIZE, &old_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Resolve destination path. For the destination, we need the wide path
  // and optionally a RootDirectory handle for the rename info struct.
  auto new_s2 = path_scratch();
  if (!new_s2)
    return -ENOMEM;
  WCHAR *new_buf = new_s2.data();
  size_t new_wide_len = to_wide_path(new_sv, new_buf, new_s2.size());
  if (new_wide_len == 0)
    return -EINVAL;

  bool new_is_absolute = is_absolute_path(new_buf, new_wide_len);

  HANDLE new_root = nullptr;
  WCHAR *new_name = new_buf;
  size_t new_name_len = new_wide_len;

  if (newdfd == AT_FDCWD || new_is_absolute) {
    // Absolute or CWD-relative: convert to full NT path for the rename.
    auto nt_newat = to_nt_path(new_sv, new_buf, new_s2.size());
    if (!nt_newat.has_value())
      return -nt_newat.error();
    new_name_len = nt_newat.value();
    new_name = new_buf;
    new_root = nullptr;
  } else {
    // Directory-relative: use the dirfd handle as RootDirectory in the
    // rename info. The FileName is the relative wide path.
    auto dir_handle = internal::fd_table.get(newdfd);
    if (!dir_handle.has_value())
      return -dir_handle.error();
    new_root = dir_handle.value();
  }

  // Build FILE_RENAME_INFORMATION with the destination path inline.
  ULONG name_bytes = static_cast<ULONG>(new_name_len * sizeof(WCHAR));
  ULONG info_size =
      static_cast<ULONG>(offsetof(FILE_RENAME_INFORMATION, FileName)) +
      name_bytes;

  auto info_s = info_scratch<FILE_RENAME_INFORMATION>();
  if (!info_s)
    return -ENOMEM;
  auto *info = reinterpret_cast<FILE_RENAME_INFORMATION *>(info_s.data());

  info->Flags = FILE_RENAME_REPLACE_IF_EXISTS | FILE_RENAME_POSIX_SEMANTICS;
  info->RootDirectory = new_root;
  info->FileNameLength = name_bytes;
  __builtin_memcpy(info->FileName, new_name, name_bytes);

  status = ::NtSetInformationFile(handle.get(), &iosb, info, info_size,
                                  FileRenameInformationEx);

  if (NT_SUCCESS(status))
    return 0;

  return -windows_util::ntstatus_to_errno(status);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
