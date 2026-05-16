//===-- Windows kernel functions for path operations -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Path-based kernel functions for Windows. These implement Linux syscall
// semantics in userspace: 0 on success (or positive value for counts),
// -errno on failure. Called from syscall_wrappers/ after constant folding.
//
// Functions in this file:
//   - faccessat: ACL-based accessibility check via NtOpenFile
//   - unlinkat: POSIX-semantics unlink via NtSetInformationFile
//   - linkat: hard link via NtSetInformationFile(FileLinkInformationEx)
//   - symlinkat: NTFS symlink via NtCreateFile + FSCTL_SET_REPARSE_POINT
//   - readlinkat: read symlink target via FSCTL_GET_REPARSE_POINT
//   - chdir: set CWD via RtlSetCurrentDirectory_U
//   - fchdir: resolve fd handle to DOS path then set CWD
//   - getcwd: read CWD from PEB + UTF-16 to UTF-8 conversion
//
//===----------------------------------------------------------------------===//

#include "path_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/string_view.h"
#include "hdr/fcntl_macros.h"
#include "hdr/func/free.h"
#include "hdr/func/malloc.h"
#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/OSUtil/windows/device_path.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/nt_wchar_converter.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/CPP/span.h"
#include "src/__support/error_or.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/OSUtil/windows/syscall.h"
#include "src/__support/macros/config.h"

#include <unistd.h> // F_OK, R_OK, W_OK, X_OK

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t faccessat(int dfd, const char *path, int amode, int flag) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;

  if (flag & AT_EACCESS) {
    return -ENOTSUP;
  }

  string_view sv(path);

  // AT_EMPTY_PATH + F_OK: the fd itself proves existence — no open needed.
  if ((flag & AT_EMPTY_PATH) && sv.empty()) {
    if (amode == F_OK) {
      if (!fd_table.get_ofd(dfd))
        return -EBADF;
      return 0;
    }
    // R_OK/W_OK/X_OK with AT_EMPTY_PATH: fall through to the re-open
    // path below — resolve_at_path will set up the empty-path re-open.
  }

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name;

  int err = resolve_at_path(dfd, sv, path_s.data(), path_s.size(), &oa, &name,
                            flag);
  if (err)
    return -err;

  if (flag & AT_SYMLINK_NOFOLLOW)
    oa.Attributes |= OBJ_DONT_REPARSE;

  // Map amode to NT access mask. F_OK only needs attribute read.
  ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
  if (amode & R_OK)
    access |= FILE_READ_DATA;
  if (amode & W_OK)
    access |= FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES;
  if (amode & X_OK)
    access |= FILE_EXECUTE;

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), access, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

intptr_t unlinkat(int dfd, const char *path, int flags) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  string_view sv(path);

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name;

  int err = resolve_at_path(dfd, sv, path_s.data(), path_s.size(), &oa, &name);
  if (err)
    return -err;

  // FILE_DIRECTORY_FILE forces the open to fail with STATUS_NOT_A_DIRECTORY
  // on non-directories (and vice versa for FILE_NON_DIRECTORY_FILE), giving
  // us the correct EISDIR/ENOTDIR errno without a separate stat check.
  ULONG open_options = FILE_SYNCHRONOUS_IO_NONALERT;
  if (flags & AT_REMOVEDIR) {
    open_options |= FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT;
  } else {
    // FILE_OPEN_REPARSE_POINT: unlink the symlink itself, not its target.
    open_options |= FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT;
  }

  // Also request READ_CONTROL + FILE_READ_EA for sticky bit check.
  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), DELETE_ACCESS | READ_CONTROL | FILE_READ_EA | SYNCHRONIZE,
      &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, open_options);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Sticky bit enforcement: open the parent directory via ".." relative
  // to the file handle and check S_ISVTX.
  {
    WCHAR dotdot[] = u"..";
    windows::nt_wstring_view parent_name(dotdot);

    auto parent_oa = windows::named_internal_oa(&parent_name, handle.get());

    windows::ScopedNtHandle parent_handle;
    IO_STATUS_BLOCK parent_iosb = {};
    NTSTATUS pst = ::NtOpenFile(
        parent_handle.put(),
        READ_CONTROL | FILE_READ_EA | SYNCHRONIZE,
        &parent_oa, &parent_iosb,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);

    if (NT_SUCCESS(pst)) {
      int sticky_err = windows_sec::check_sticky_bit(parent_handle.get(), handle.get());
      if (sticky_err) {
        return -sticky_err;
      }
    }
    // If parent open fails, skip sticky check (fail-open).
  }

  FILE_DISPOSITION_INFORMATION_EX disp = {};
  disp.Flags = FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS;
  status = ::NtSetInformationFile(handle.get(), &iosb, &disp,
                                  static_cast<ULONG>(sizeof(disp)),
                                  FileDispositionInformationEx);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

intptr_t linkat(int olddfd, const char *oldpath, int newdfd, const char *newpath,
            int flags) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!oldpath || !newpath)
    return -EFAULT;
  string_view old_sv(oldpath);
  string_view new_sv(newpath);

  // Open the source. AT_SYMLINK_FOLLOW means follow symlinks (hard-link
  // the target); default is to hard-link the symlink itself.
  auto old_s = path_scratch();
  if (!old_s) return -ENOMEM;
  OBJECT_ATTRIBUTES old_oa;
  windows::nt_wstring_view old_name;

  int err = resolve_at_path(olddfd, old_sv, old_s.data(), old_s.size(),
                            &old_oa, &old_name, flags);
  if (err)
    return -err;

  ULONG open_options = FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT;
  if (!(flags & AT_SYMLINK_FOLLOW))
    open_options |= FILE_OPEN_REPARSE_POINT;

  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, &old_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, open_options);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Resolve destination path.
  auto new_s = path_scratch();
  if (!new_s)
    return -ENOMEM;
  WCHAR *new_buf = new_s.data();
  size_t new_wide_len = to_wide_path(new_sv, new_buf, new_s.size());
  if (new_wide_len == 0)
    return -EINVAL;

  bool new_is_absolute = is_absolute_path(new_buf, new_wide_len);
  HANDLE new_root = nullptr;
  WCHAR *new_name = new_buf;
  size_t new_name_len = new_wide_len;

  if (newdfd == AT_FDCWD || new_is_absolute) {
    auto nt = to_nt_path(new_sv, new_buf, new_s.size());
    if (!nt.has_value())
      return -nt.error();
    new_name_len = nt.value();
  } else {
    auto dir_handle = internal::fd_table.get(newdfd);
    if (!dir_handle.has_value())
      return -dir_handle.error();
    new_root = dir_handle.value();
  }

  // Build FILE_LINK_INFORMATION for FileLinkInformationEx (RS5+).
  ULONG name_bytes = static_cast<ULONG>(new_name_len * sizeof(WCHAR));
  ULONG info_size =
      static_cast<ULONG>(offsetof(FILE_LINK_INFORMATION, FileName)) +
      name_bytes;

  auto info_s = info_scratch<FILE_LINK_INFORMATION>();
  if (!info_s)
    return -ENOMEM;
  auto *info = reinterpret_cast<FILE_LINK_INFORMATION *>(info_s.data());

  info->Flags = FILE_LINK_POSIX_SEMANTICS;
  info->RootDirectory = new_root;
  info->FileNameLength = name_bytes;
  __builtin_memcpy(info->FileName, new_name, name_bytes);

  status = ::NtSetInformationFile(handle.get(), &iosb, info, info_size,
                                  FileLinkInformationEx);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

intptr_t symlinkat(const char *target, int newdfd, const char *linkpath) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!target || !linkpath)
    return -EFAULT;
  string_view target_sv(target);
  string_view link_sv(linkpath);

  // Resolve the link path (where the symlink will be created).
  auto link_s = path_scratch();
  if (!link_s) return -ENOMEM;
  OBJECT_ATTRIBUTES link_oa;
  windows::nt_wstring_view link_name;

  int err = resolve_at_path(newdfd, link_sv, link_s.data(), link_s.size(),
                            &link_oa, &link_name);
  if (err)
    return -err;

  // Convert the target path to wide. This is the path stored inside the
  // reparse point — it does not get the \??\ prefix since it's the
  // symbolic link payload, not an NT object path.
  auto target_s = path_scratch();
  if (!target_s) return -ENOMEM;
  WCHAR *target_buf = target_s.data();
  size_t target_len = to_wide_path(target_sv, target_buf, target_s.size());
  if (target_len == 0)
    return -EINVAL;

  // Determine if the target is relative (for SYMLINK_FLAG_RELATIVE).
  bool target_is_relative = !is_absolute_path(target_buf, target_len);

  // Build the substitute name (what the OS resolves) and print name
  // (what readlink returns). For relative symlinks, both are the same.
  // For absolute symlinks, the substitute name gets the \??\ prefix.
  auto sub_s = path_scratch();
  if (!sub_s) return -ENOMEM;
  WCHAR *sub_buf = sub_s.data();
  WCHAR *sub_name = target_buf;
  size_t sub_len = target_len;

  if (!target_is_relative) {
    // Absolute target — substitute name needs \??\ prefix.
    windows::WStringStream ss(cpp::span<WCHAR>(sub_buf, sub_s.size() - 1));
    ss << u"\\?\\" << windows::nt_wstring_view(target_buf, target_len);
    ss.null_terminate();
    sub_name = sub_buf;
    sub_len = ss.str().size();
  }

  // Build REPARSE_DATA_BUFFER.
  USHORT sub_bytes = static_cast<USHORT>(sub_len * sizeof(WCHAR));
  USHORT print_bytes = static_cast<USHORT>(target_len * sizeof(WCHAR));
  ULONG reparse_data_len =
      offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer) -
      offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer) +
      sub_bytes + print_bytes;
  ULONG reparse_total =
      offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer) +
      sub_bytes + print_bytes;

  auto reparse_s = byte_scratch(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  if (!reparse_s) return -ENOMEM;
  auto *reparse = reinterpret_cast<REPARSE_DATA_BUFFER *>(reparse_s.data());

  reparse->ReparseTag = IO_REPARSE_TAG_SYMLINK;
  reparse->ReparseDataLength = static_cast<USHORT>(reparse_data_len);
  reparse->Reserved = 0;

  auto &sym = reparse->SymbolicLinkReparseBuffer;
  sym.SubstituteNameOffset = 0;
  sym.SubstituteNameLength = sub_bytes;
  sym.PrintNameOffset = sub_bytes;
  sym.PrintNameLength = print_bytes;
  sym.Flags = target_is_relative ? SYMLINK_FLAG_RELATIVE : 0;
  __builtin_memcpy(sym.PathBuffer, sub_name, sub_bytes);
  __builtin_memcpy(reinterpret_cast<char *>(sym.PathBuffer) + sub_bytes,
                   target_buf, print_bytes);

  // Create the symlink file.
  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtCreateFile(
      handle.put(),
      FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE_ACCESS | SYNCHRONIZE,
      &link_oa, &iosb,
      nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_CREATE, // fail if exists
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
          FILE_OPEN_REPARSE_POINT,
      nullptr, 0);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Set the reparse point data.
  status = ::NtFsControlFile(handle.get(), nullptr, nullptr, nullptr, &iosb,
                              FSCTL_SET_REPARSE_POINT, reparse, reparse_total,
                              nullptr, 0);
  if (!NT_SUCCESS(status)) {
    // Clean up the empty file we created.
    FILE_DISPOSITION_INFORMATION_EX disp = {};
    disp.Flags = FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS;
    IO_STATUS_BLOCK disp_iosb = {};
    ::NtSetInformationFile(handle.get(), &disp_iosb, &disp, sizeof(disp),
                           FileDispositionInformationEx);
    return -windows_util::ntstatus_to_errno(status);
  }

  // Stamp ownership EAs ($LXMOD/$LXUID/$LXGID). Symlinks get 0777 mode.
  windows_sec::post_create_perms(handle.get(), 0777);

  return 0;
}

intptr_t readlinkat(int dfd, const char *__restrict path, char *__restrict buf,
                size_t bufsiz) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  string_view sv(path);

  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name;

  int err = resolve_at_path(dfd, sv, path_s.data(), path_s.size(), &oa, &name);
  if (err)
    return -err;

  // Open the reparse point itself, not its target.
  windows::ScopedNtHandle handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      handle.put(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Read the reparse data.
  auto reparse_s = byte_scratch(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  if (!reparse_s)
    return -ENOMEM;
  auto *reparse = reinterpret_cast<REPARSE_DATA_BUFFER *>(reparse_s.data());

  status = ::NtFsControlFile(handle.get(), nullptr, nullptr, nullptr, &iosb,
                              FSCTL_GET_REPARSE_POINT, nullptr, 0,
                              reparse, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);

  if (!NT_SUCCESS(status)) {
    return -(status == STATUS_NOT_A_REPARSE_POINT
                 ? EINVAL
                 : windows_util::ntstatus_to_errno(status));
  }

  if (reparse->ReparseTag != IO_REPARSE_TAG_SYMLINK)
    return -EINVAL;

  // Extract the substitute name from the reparse buffer.
  auto &sym = reparse->SymbolicLinkReparseBuffer;
  WCHAR *sub_name = sym.PathBuffer + (sym.SubstituteNameOffset / sizeof(WCHAR));
  USHORT sub_len_chars = sym.SubstituteNameLength / sizeof(WCHAR);

  // Strip the \??\ prefix if present — convert NT path back to DOS path.
  if (sub_len_chars >= 4 && sub_name[0] == u'\\' && sub_name[1] == u'?' &&
      sub_name[2] == u'?' && sub_name[3] == u'\\') {
    sub_name += 4;
    sub_len_chars -= 4;
  }

  // Convert UTF-16 to UTF-8.
  int utf8_len =
      windows::wide_to_utf8_n(sub_name, sub_len_chars, buf, bufsiz);
  if (utf8_len < 0)
    return -EINVAL;

  // POSIX: readlink does not null-terminate. Return byte count.
  return static_cast<intptr_t>(utf8_len);
}

intptr_t chdir(const char *path) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (path == nullptr)
    return -EFAULT;
  string_view sv(path);

  // Convert UTF-8 to wide (no NT prefix — RtlSetCurrentDirectory_U expects
  // DOS paths). MAX_NT_PATH_WCHARS matches the UNICODE_STRING USHORT ceiling.
  auto wide_s = path_scratch();
  if (!wide_s) return -ENOMEM;
  WCHAR *wide_buf = wide_s.data();
  size_t wide_chars = to_wide_path(sv, wide_buf, wide_s.size());
  if (wide_chars == 0)
    return -EINVAL;

  // Normalise forward slashes to backslashes.
  for (size_t i = 0; i < wide_chars; ++i) {
    if (wide_buf[i] == u'/')
      wide_buf[i] = u'\\';
  }

  windows::nt_wstring_view name(wide_buf, wide_chars);

  NTSTATUS status = ::RtlSetCurrentDirectory_U(name.unicode_string());
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

intptr_t fchdir(int fd) {
  // Look up the handle from our fd table.
  internal::OpenFileDescription *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;

  HANDLE handle = ofd->handle;
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    return -EBADF;

  // Query the current NT object name from the handle.
  auto name_s = info_scratch<OBJECT_NAME_INFORMATION>();
  if (!name_s) return -ENOMEM;
  ULONG returned = 0;
  NTSTATUS status = ::NtQueryObject(
      handle, ObjectNameInformation, name_s.data(),
      static_cast<ULONG>(name_s.size()), &returned);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  auto *name_info = reinterpret_cast<OBJECT_NAME_INFORMATION *>(name_s.data());
  WCHAR *nt_path = name_info->Name.Buffer;
  ULONG nt_path_chars = name_info->Name.Length / sizeof(WCHAR);

  // Resolve the NT device path to a DOS path via the mount manager.
  auto dos_s = path_scratch();
  if (!dos_s) return -ENOMEM;
  WCHAR *dos_buf = dos_s.data();
  size_t dos_len = windows_util::device_path_to_dos(
      nt_path, nt_path_chars, dos_buf, dos_s.size());
  if (dos_len == 0)
    return -ENOENT;

  // Set the current directory.
  windows::nt_wstring_view name(dos_buf, dos_len);

  status = ::RtlSetCurrentDirectory_U(name.unicode_string());
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

intptr_t getcwd(char *buf, size_t size) {
  if (buf != nullptr && size == 0)
    return -EINVAL;

  // Read CWD directly from PEB — UTF-16.
  PEB *peb = NtCurrentPeb();
  UNICODE_STRING *cwd = &peb->ProcessParameters->CurrentDirectory.DosPath;

  // Strip trailing backslash unless it's a root like "C:\".
  ULONG wide_bytes = cwd->Length;
  ULONG wide_chars = wide_bytes / sizeof(WCHAR);
  if (wide_chars > 3 && cwd->Buffer[wide_chars - 1] == u'\\')
    wide_bytes -= sizeof(WCHAR);

  // Query required UTF-8 size (two-pass: NULL dest returns needed count).
  int utf8_len = windows::utf16_to_utf8(cwd->Buffer,
                                         wide_bytes / sizeof(WCHAR),
                                         nullptr, 0);
  if (utf8_len < 0)
    return -EINVAL;

  ULONG needed = static_cast<ULONG>(utf8_len) + 1; // +1 for NUL

  bool allocated = false;
  if (buf == nullptr) {
    // glibc extension: allocate a buffer.
    size = (size > needed) ? size : needed;
    buf = static_cast<char *>(::malloc(size));
    if (buf == nullptr)
      return -ENOMEM;
    allocated = true;
  } else if (size < needed) {
    return -ERANGE;
  }

  // Convert UTF-16 to UTF-8.
  int actual = windows::utf16_to_utf8(cwd->Buffer,
                                       wide_bytes / sizeof(WCHAR),
                                       buf, size - 1);
  if (actual < 0) {
    if (allocated)
      ::free(buf);
    return -EINVAL;
  }

  // Convert backslashes to forward slashes for POSIX consistency.
  for (int i = 0; i < actual; ++i) {
    if (buf[i] == '\\')
      buf[i] = '/';
  }

  buf[actual] = '\0';
  return reinterpret_cast<intptr_t>(buf);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
