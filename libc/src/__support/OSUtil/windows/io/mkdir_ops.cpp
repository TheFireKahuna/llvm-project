//===-- Windows internal mkdirat/mkfifo operations -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for mkdirat and mkfifo on Windows. These implement
// Linux syscall semantics: 0 on success, -errno on failure.
// Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "mkdir_ops.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/fifo.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/error_or.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t mkdirat(int dfd, const char *path, mode_t mode) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  string_view sv(path);

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name;

  int err = resolve_at_path(dfd, sv, path_buf, path_buf_s.size(), &oa, &name);
  if (err)
    return -err;

  mode_t effective = mode & ~windows_sec::get_umask();

  // Build SD with POSIX DACL. Pass the parent directory handle for
  // S_ISGID inheritance (new dirs inherit the parent's group).
  // oa.RootDirectory is the parent for dirfd-relative paths.
  auto sd_s = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
  if (!sd_s)
    return -ENOMEM;
  mode_t final_mode = effective;
  SECURITY_DESCRIPTOR *sd = windows_sec::build_creation_sd(
      reinterpret_cast<UCHAR *>(sd_s.data()), effective, oa.RootDirectory,
      &final_mode);
  oa.SecurityDescriptor = sd;

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtCreateFile(
      handle.put(),
      FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_WRITE_ATTRIBUTES |
          FILE_WRITE_EA | SYNCHRONIZE,
      &oa, &iosb,
      nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_CREATE,
      FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // If parent had S_ISGID, propagate it to this new directory and
  // record the inherited group gid.
  gid_t inherited_gid = static_cast<gid_t>(-1);
  if (oa.RootDirectory) {
    gid_t parent_gid = 0;
    alignas(4) UCHAR gsid_buf[windows_sec::MAX_SID_SIZE];
    SID *gsid = reinterpret_cast<SID *>(gsid_buf);
    if (windows_sec::query_parent_sgid(oa.RootDirectory, &parent_gid, gsid)) {
      final_mode |= S_ISGID; // Propagate to new directory.
      inherited_gid = parent_gid;
    }
  }

  windows_sec::post_create_perms(handle.get(), final_mode, inherited_gid);
  return 0;
}

intptr_t mkfifo(const char *path, mode_t mode) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  string_view sv(path);

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  auto nt = to_nt_path(sv, path_buf, path_buf_s.size());
  if (!nt.has_value())
    return -nt.error();
  size_t path_len = nt.value();

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name(path_buf, path_len);
  init_object_attributes(&oa, &name);

  // Apply umask and build atomic SD for the marker file.
  mode_t effective = mode & ~windows_sec::get_umask();
  auto sd_s2 = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
  if (!sd_s2)
    return -ENOMEM;
  oa.SecurityDescriptor = windows_sec::build_creation_sd(
      reinterpret_cast<UCHAR *>(sd_s2.data()), effective);

  IO_STATUS_BLOCK iosb = {};
  HANDLE h;

  // Create the marker file. FILE_CREATE fails with STATUS_OBJECT_NAME_COLLISION
  // if the path already exists (→ EEXIST).
  NTSTATUS status = NtCreateFile(
      &h,
      FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      &oa, &iosb,
      nullptr,
      FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_CREATE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Write magic + mode. The race window (file exists, magic not yet written)
  // is harmless — open() won't recognize it as a FIFO until the magic lands.
  status = write_fifo_marker(h, mode);

  // EA + READONLY — DACL was set atomically via SecurityDescriptor.
  if (NT_SUCCESS(status))
    windows_sec::post_create_perms(h, effective);

  NtClose(h);

  if (!NT_SUCCESS(status)) {
    // Clean up the empty file on write failure.
    NtDeleteFile(&oa);
    return -EIO;
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
