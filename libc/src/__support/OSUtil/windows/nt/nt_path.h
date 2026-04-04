//===-- NT path and object-name utilities -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Convert UTF-8 file paths to NT object namespace format (\??\C:\...) and
// build UNICODE_STRING + OBJECT_ATTRIBUTES values for NT syscalls.
//
// This header also contains small helpers for NT object-manager names such as
// \BaseNamedObjects\<leaf>. These are intentionally kept separate from DOS file
// path conversion because object names are not filesystem paths and should not
// go through \??\ path normalization.
//
// Shared by the FILE* layer (openfile) and the fd layer (internal::open).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_H

#include "hdr/fcntl_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_path_convert.h"
#include "src/__support/OSUtil/windows/nt/nt_wchar_converter.h"
#include "src/__support/OSUtil/windows/nt/session_bno.h"
#include "src/__support/macros/attributes.h"

// Initialize OBJECT_ATTRIBUTES from a nt_wstring_view. The view's internal
// UNICODE_STRING is used directly — no copy, no conversion. The nt_wstring_view
// must outlive the OBJECT_ATTRIBUTES.
LIBC_INLINE void init_object_attributes(OBJECT_ATTRIBUTES *oa,
                                        LIBC_NAMESPACE::windows::nt_wstring_view *name) {
  oa->Length = sizeof(OBJECT_ATTRIBUTES);
  oa->RootDirectory = nullptr;
  oa->ObjectName = name->unicode_string();
  oa->Attributes = OBJ_CASE_INSENSITIVE | OBJ_INHERIT;
  oa->SecurityDescriptor = nullptr;
  oa->SecurityQualityOfService = nullptr;
}

// Initialize OBJECT_ATTRIBUTES with a RootDirectory handle for directory-
// relative operations (*at() family). When root_dir is non-null, ObjectName
// is interpreted relative to it by the NT object manager.
LIBC_INLINE void init_object_attributes_at(OBJECT_ATTRIBUTES *oa,
                                           LIBC_NAMESPACE::windows::nt_wstring_view *name,
                                           HANDLE root_dir) {
  init_object_attributes(oa, name);
  oa->RootDirectory = root_dir;
}

// Check whether a wide path is absolute (has a drive letter like C:\ or
// starts with a backslash from a POSIX absolute path like /tmp).
// Used by *at() functions to decide whether to use RootDirectory.
LIBC_INLINE bool is_absolute_path(const WCHAR *path, size_t len) {
  if (len >= 3 && path[1] == u':' &&
      (path[2] == u'\\' || path[2] == u'/'))
    return true;
  // POSIX absolute paths start with / which becomes \ after wide conversion.
  if (len >= 1 && (path[0] == u'\\' || path[0] == u'/'))
    return true;
  return false;
}

// Convert UTF-8 to wide without any NT prefix. Returns length in WCHARs
// (excluding NUL), or 0 on failure. For use with RootDirectory-relative
// paths in *at() functions.
LIBC_INLINE size_t to_wide_path(LIBC_NAMESPACE::cpp::string_view path,
                                WCHAR *buf, size_t max_wchars) {
  int wide_result =
      LIBC_NAMESPACE::windows::utf8_to_utf16(path.data(), path.size(), buf,
                                             max_wchars);
  if (wide_result < 0)
    return 0;
  size_t wide_chars = static_cast<size_t>(wide_result);
  if (wide_chars >= max_wchars)
    return 0;

  // NT Object Manager only recognizes backslash as a path separator.
  for (size_t i = 0; i < wide_chars; ++i)
    if (buf[i] == u'/')
      buf[i] = u'\\';

  buf[wide_chars] = u'\0';
  return wide_chars;
}

// Returns true if |name| is a non-empty NT object leaf name. This excludes
// slash-prefixed POSIX names and any path-like name containing '/' or '\'.
LIBC_INLINE bool is_nt_object_leaf_name(LIBC_NAMESPACE::cpp::string_view name) {
  if (name.empty() || name[0] == '/')
    return false;

  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '/' || name[i] == '\\')
      return false;
  }

  return true;
}

// Convert a UTF-8 NT object leaf name into a session-scoped NT object path:
// \Sessions\<SessionId>\BaseNamedObjects\<leaf>.
//
// Non-admin user processes cannot create objects directly in the global
// \BaseNamedObjects directory; that operation requires SeCreateGlobalPrivilege
// and fails with STATUS_ACCESS_DENIED from ordinary processes. All libc-owned
// named NT objects (SysV IPC, FIFOs, POSIX semaphores, ALPC ports) are
// session-scoped by design, matching POSIX isolation semantics.
//
// Returns the resulting WCHAR length excluding NUL, or 0 on failure.
LIBC_INLINE size_t
to_base_named_object_path(LIBC_NAMESPACE::cpp::string_view leaf, WCHAR *buf,
                          size_t max_wchars) {
  if (!is_nt_object_leaf_name(leaf))
    return 0;
  return LIBC_NAMESPACE::windows::to_session_bno_path(leaf, buf, max_wchars);
}

// Resolve a dirfd + path pair into OBJECT_ATTRIBUTES for *at() functions.
// Handles AT_FDCWD, absolute paths (ignore dirfd), and directory-relative
// paths (set RootDirectory). Returns 0 on success, errno value on failure.
// Null checking is the caller's responsibility.
LIBC_INLINE int resolve_at_path(int dirfd, LIBC_NAMESPACE::cpp::string_view path,
                                WCHAR *path_buf, size_t buf_wchars,
                                OBJECT_ATTRIBUTES *oa,
                                LIBC_NAMESPACE::windows::nt_wstring_view *name) {
  size_t wide_len = to_wide_path(path, path_buf, buf_wchars);
  if (wide_len == 0)
    return EINVAL;

  if (dirfd == AT_FDCWD || is_absolute_path(path_buf, wide_len)) {
    // resolve_path handles all path types and returns proper error codes
    // (ENAMETOOLONG, ENOENT for empty paths, etc.).
    ResolvedPath rp = path_detail::resolve_path(path, path_buf, buf_wchars);
    if (rp.error)
      return rp.error;
    if (rp.nt_len == 0)
      return EINVAL; // Virtual-only paths (DevPty, DevFd) need special handling.
    *name = LIBC_NAMESPACE::windows::nt_wstring_view(path_buf, rp.nt_len);
    init_object_attributes(oa, name);
  } else {
    auto dir_handle = LIBC_NAMESPACE::internal::fd_table.get(dirfd);
    if (!dir_handle.has_value())
      return dir_handle.error();
    *name = LIBC_NAMESPACE::windows::nt_wstring_view(path_buf, wide_len);
    init_object_attributes_at(oa, name, dir_handle.value());
  }
  return 0;
}

// AT_EMPTY_PATH-aware overload. When at_flags contains AT_EMPTY_PATH and
// path is empty, sets up OBJECT_ATTRIBUTES to reopen the dirfd's file
// object. NtOpenFile with RootDirectory set and an empty ObjectName opens
// the same underlying file — this is the canonical NT pattern for upgrading
// a handle's access rights (e.g. from a FILE_READ_ATTRIBUTES-only O_PATH
// handle to one with WRITE_DAC for fchmodat).
//
// For all other cases, delegates to the regular resolve_at_path.
// Null checking is the caller's responsibility.
LIBC_INLINE int resolve_at_path(int dirfd, LIBC_NAMESPACE::cpp::string_view path,
                                WCHAR *path_buf, size_t buf_wchars,
                                OBJECT_ATTRIBUTES *oa,
                                LIBC_NAMESPACE::windows::nt_wstring_view *name,
                                int at_flags) {
  if ((at_flags & AT_EMPTY_PATH) && path.empty()) {
    if (dirfd == AT_FDCWD)
      return EINVAL; // AT_EMPTY_PATH with AT_FDCWD and "" is invalid.
    auto dir_handle = LIBC_NAMESPACE::internal::fd_table.get(dirfd);
    if (!dir_handle.has_value())
      return dir_handle.error();
    // Empty ObjectName + RootDirectory = reopen the same file object.
    *name = LIBC_NAMESPACE::windows::nt_wstring_view(path_buf, 0);
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->RootDirectory = dir_handle.value();
    oa->ObjectName = name->unicode_string();
    oa->Attributes = OBJ_CASE_INSENSITIVE;
    oa->SecurityDescriptor = nullptr;
    oa->SecurityQualityOfService = nullptr;
    return 0;
  }
  return resolve_at_path(dirfd, path, path_buf, buf_wchars, oa, name);
}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_H
