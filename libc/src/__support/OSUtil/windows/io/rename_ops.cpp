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
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t remove_file(const char *path) {
  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  size_t path_len = to_nt_path(path, path_s.data(), path_s.size());
  if (path_len == 0)
    return -EINVAL;

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, path_s.data(), path_len);

  // Try file deletion first (single syscall, no handle needed).
  NTSTATUS status = ::NtDeleteFile(&oa);
  if (NT_SUCCESS(status))
    return 0;

  // NtDeleteFile fails on directories — open and delete via disposition.
  if (status == STATUS_FILE_IS_A_DIRECTORY) {
    HANDLE handle;
    IO_STATUS_BLOCK iosb = {};
    status = ::NtOpenFile(&handle, DELETE_ACCESS | SYNCHRONIZE, &oa, &iosb,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                              FILE_OPEN_FOR_BACKUP_INTENT);
    if (NT_SUCCESS(status)) {
      FILE_DISPOSITION_INFORMATION_EX disp = {};
      disp.Flags = FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS;
      status = ::NtSetInformationFile(handle, &iosb, &disp,
                                      static_cast<ULONG>(sizeof(disp)),
                                      FileDispositionInformationEx);
      ::NtClose(handle);
    }
  }

  if (NT_SUCCESS(status))
    return 0;

  return -windows_util::ntstatus_to_errno(status);
}

intptr_t rename_file(const char *oldpath, const char *newpath) {
  // Open the source file/directory for rename.
  auto old_s = path_scratch();
  if (!old_s) return -ENOMEM;
  WCHAR *old_buf = old_s.data();
  size_t old_len = to_nt_path(oldpath, old_buf, old_s.size());
  if (old_len == 0)
    return -EINVAL;

  UNICODE_STRING old_us;
  OBJECT_ATTRIBUTES old_oa;
  init_object_attributes(&old_oa, &old_us, old_buf, old_len);

  HANDLE handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      &handle, DELETE_ACCESS | SYNCHRONIZE, &old_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Convert destination path to NT format.
  auto new_s = path_scratch();
  if (!new_s) {
    ::NtClose(handle);
    return -ENOMEM;
  }
  WCHAR *new_buf = new_s.data();
  size_t new_len = to_nt_path(newpath, new_buf, new_s.size());
  if (new_len == 0) {
    ::NtClose(handle);
    return -EINVAL;
  }

  // Build FILE_RENAME_INFORMATION with the destination path inline.
  // Use FileRenameInformationEx for POSIX semantics (atomic replace).
  ULONG name_bytes = static_cast<ULONG>(new_len * sizeof(WCHAR));
  ULONG info_size =
      static_cast<ULONG>(offsetof(FILE_RENAME_INFORMATION, FileName)) +
      name_bytes;

  auto info_s = info_scratch<FILE_RENAME_INFORMATION>();
  if (!info_s) {
    ::NtClose(handle);
    return -ENOMEM;
  }
  auto *info = reinterpret_cast<FILE_RENAME_INFORMATION *>(info_s.data());

  info->Flags = FILE_RENAME_REPLACE_IF_EXISTS | FILE_RENAME_POSIX_SEMANTICS;
  info->RootDirectory = nullptr;
  info->FileNameLength = name_bytes;
  __builtin_memcpy(info->FileName, new_buf, name_bytes);

  status = ::NtSetInformationFile(handle, &iosb, info, info_size,
                                  FileRenameInformationEx);
  ::NtClose(handle);

  if (NT_SUCCESS(status))
    return 0;

  return -windows_util::ntstatus_to_errno(status);
}

intptr_t renameat(int olddfd, const char *oldpath,
              int newdfd, const char *newpath) {
  // Resolve source path.
  auto old_s2 = path_scratch();
  if (!old_s2) return -ENOMEM;
  UNICODE_STRING old_us;
  OBJECT_ATTRIBUTES old_oa;

  int err = resolve_at_path(olddfd, oldpath, old_s2.data(), old_s2.size(),
                            &old_oa, &old_us);
  if (err)
    return -err;

  // Open source for rename. FILE_OPEN_REPARSE_POINT renames the symlink
  // itself, not its target.
  HANDLE handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      &handle, DELETE_ACCESS | SYNCHRONIZE, &old_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Resolve destination path. For the destination, we need the wide path
  // and optionally a RootDirectory handle for the rename info struct.
  auto new_s2 = path_scratch();
  if (!new_s2) {
    ::NtClose(handle);
    return -ENOMEM;
  }
  WCHAR *new_buf = new_s2.data();
  size_t new_wide_len = to_wide_path(newpath, new_buf, new_s2.size());
  if (new_wide_len == 0) {
    ::NtClose(handle);
    return -EINVAL;
  }

  bool new_is_absolute = is_absolute_path(new_buf, new_wide_len);

  HANDLE new_root = nullptr;
  WCHAR *new_name = new_buf;
  size_t new_name_len = new_wide_len;

  if (newdfd == AT_FDCWD || new_is_absolute) {
    // Absolute or CWD-relative: convert to full NT path for the rename.
    new_name_len = to_nt_path(newpath, new_buf, new_s2.size());
    if (new_name_len == 0) {
      ::NtClose(handle);
      return -EINVAL;
    }
    new_name = new_buf;
    new_root = nullptr;
  } else {
    // Directory-relative: use the dirfd handle as RootDirectory in the
    // rename info. The FileName is the relative wide path.
    auto dir_handle = internal::fd_table.get(newdfd);
    if (!dir_handle.has_value()) {
      ::NtClose(handle);
      return -dir_handle.error();
    }
    new_root = dir_handle.value();
  }

  // Build FILE_RENAME_INFORMATION with the destination path inline.
  ULONG name_bytes = static_cast<ULONG>(new_name_len * sizeof(WCHAR));
  ULONG info_size =
      static_cast<ULONG>(offsetof(FILE_RENAME_INFORMATION, FileName)) +
      name_bytes;

  auto info_s = info_scratch<FILE_RENAME_INFORMATION>();
  if (!info_s) {
    ::NtClose(handle);
    return -ENOMEM;
  }
  auto *info = reinterpret_cast<FILE_RENAME_INFORMATION *>(info_s.data());

  info->Flags = FILE_RENAME_REPLACE_IF_EXISTS | FILE_RENAME_POSIX_SEMANTICS;
  info->RootDirectory = new_root;
  info->FileNameLength = name_bytes;
  __builtin_memcpy(info->FileName, new_name, name_bytes);

  status = ::NtSetInformationFile(handle, &iosb, info, info_size,
                                  FileRenameInformationEx);
  ::NtClose(handle);

  if (NT_SUCCESS(status))
    return 0;

  return -windows_util::ntstatus_to_errno(status);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
