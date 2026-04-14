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
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_path_convert.h"
#include "src/__support/macros/attributes.h"

// Initialize UNICODE_STRING and OBJECT_ATTRIBUTES from a wide path buffer.
LIBC_INLINE void init_object_attributes(OBJECT_ATTRIBUTES *oa,
                                        UNICODE_STRING *us,
                                        const WCHAR *path_buf,
                                        size_t path_len) {
  us->Length = static_cast<USHORT>(path_len * sizeof(WCHAR));
  us->MaximumLength = static_cast<USHORT>((path_len + 1) * sizeof(WCHAR));
  us->Buffer = const_cast<WCHAR *>(path_buf);

  oa->Length = sizeof(OBJECT_ATTRIBUTES);
  oa->RootDirectory = nullptr;
  oa->ObjectName = us;
  oa->Attributes = OBJ_CASE_INSENSITIVE | OBJ_INHERIT;
  oa->SecurityDescriptor = nullptr;
  oa->SecurityQualityOfService = nullptr;
}

// Initialize OBJECT_ATTRIBUTES with a RootDirectory handle for directory-
// relative operations (*at() family). When root_dir is non-null, ObjectName
// is interpreted relative to it by the NT object manager.
LIBC_INLINE void init_object_attributes_at(OBJECT_ATTRIBUTES *oa,
                                           UNICODE_STRING *us,
                                           HANDLE root_dir,
                                           const WCHAR *path_buf,
                                           size_t path_len) {
  init_object_attributes(oa, us, path_buf, path_len);
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
LIBC_INLINE size_t to_wide_path(const char *path, WCHAR *buf,
                                 size_t max_wchars) {
  ULONG wide_bytes = 0;
  size_t path_len = __builtin_strlen(path);
  NTSTATUS status = ::RtlUTF8ToUnicodeN(
      buf, static_cast<ULONG>(max_wchars * sizeof(WCHAR)), &wide_bytes, path,
      static_cast<ULONG>(path_len));
  if (!NT_SUCCESS(status))
    return 0;
  ULONG wide_chars = wide_bytes / sizeof(WCHAR);
  if (wide_chars >= max_wchars)
    return 0;

  // NT Object Manager only recognizes backslash as a path separator.
  for (ULONG i = 0; i < wide_chars; ++i)
    if (buf[i] == u'/')
      buf[i] = u'\\';

  buf[wide_chars] = u'\0';
  return wide_chars;
}

// Returns true if |name| is a non-empty NT object leaf name. This excludes
// slash-prefixed POSIX names and any path-like name containing '/' or '\'.
LIBC_INLINE bool is_nt_object_leaf_name(const char *name) {
  if (!name || name[0] == '\0' || name[0] == '/')
    return false;

  for (const char *p = name; *p; ++p) {
    if (*p == '/' || *p == '\\')
      return false;
  }

  return true;
}

// Convert a UTF-8 NT object leaf name into a \BaseNamedObjects\<leaf> path.
// Returns the resulting WCHAR length excluding NUL, or 0 on failure.
LIBC_INLINE size_t to_base_named_object_path(const char *leaf, WCHAR *buf,
                                             size_t max_wchars) {
  constexpr WCHAR PREFIX[] = u"\\BaseNamedObjects\\";
  constexpr size_t PREFIX_LEN = sizeof(PREFIX) / sizeof(PREFIX[0]) - 1;

  if (!is_nt_object_leaf_name(leaf))
    return 0;
  if (max_wchars <= PREFIX_LEN)
    return 0;

  ULONG wide_bytes = 0;
  size_t leaf_len = __builtin_strlen(leaf);
  ULONG avail =
      static_cast<ULONG>((max_wchars - PREFIX_LEN) * sizeof(WCHAR));
  NTSTATUS status = ::RtlUTF8ToUnicodeN(buf + PREFIX_LEN, avail, &wide_bytes,
                                        leaf, static_cast<ULONG>(leaf_len));
  if (!NT_SUCCESS(status))
    return 0;

  ULONG wide_chars = wide_bytes / sizeof(WCHAR);
  size_t total = PREFIX_LEN + wide_chars;
  if (total >= max_wchars || wide_chars == 0)
    return 0;

  for (size_t i = 0; i < PREFIX_LEN; ++i)
    buf[i] = PREFIX[i];

  buf[total] = u'\0';
  return total;
}

// Resolve a dirfd + path pair into OBJECT_ATTRIBUTES for *at() functions.
// Handles AT_FDCWD, absolute paths (ignore dirfd), and directory-relative
// paths (set RootDirectory). Returns 0 on success, errno value on failure.
LIBC_INLINE int resolve_at_path(int dirfd, const char *path, WCHAR *path_buf,
                                 size_t buf_wchars, OBJECT_ATTRIBUTES *oa,
                                 UNICODE_STRING *us) {
  size_t wide_len = to_wide_path(path, path_buf, buf_wchars);
  if (wide_len == 0)
    return EINVAL;

  if (dirfd == AT_FDCWD || is_absolute_path(path_buf, wide_len)) {
    // resolve_path handles all path types and returns proper error codes
    // (ENAMETOOLONG, ENOENT for empty paths, etc.).
    ResolvedPath rp = resolve_path(path, path_buf, buf_wchars);
    if (rp.error)
      return rp.error;
    if (rp.nt_len == 0)
      return EINVAL; // Virtual-only paths (DevPty, DevFd) need special handling.
    init_object_attributes(oa, us, path_buf, rp.nt_len);
  } else {
    auto dir_handle = LIBC_NAMESPACE::internal::fd_table.get(dirfd);
    if (!dir_handle.has_value())
      return dir_handle.error();
    init_object_attributes_at(oa, us, dir_handle.value(), path_buf, wide_len);
  }
  return 0;
}

// AT_EMPTY_PATH-aware overload. When at_flags contains AT_EMPTY_PATH and
// path is empty (""), sets up OBJECT_ATTRIBUTES to reopen the dirfd's file
// object. NtOpenFile with RootDirectory set and an empty ObjectName opens
// the same underlying file — this is the canonical NT pattern for upgrading
// a handle's access rights (e.g. from a FILE_READ_ATTRIBUTES-only O_PATH
// handle to one with WRITE_DAC for fchmodat).
//
// For all other cases, delegates to the regular resolve_at_path.
LIBC_INLINE int resolve_at_path(int dirfd, const char *path, WCHAR *path_buf,
                                size_t buf_wchars, OBJECT_ATTRIBUTES *oa,
                                UNICODE_STRING *us, int at_flags) {
  if ((at_flags & AT_EMPTY_PATH) && path[0] == '\0') {
    if (dirfd == AT_FDCWD)
      return EINVAL; // AT_EMPTY_PATH with AT_FDCWD and "" is invalid.
    auto dir_handle = LIBC_NAMESPACE::internal::fd_table.get(dirfd);
    if (!dir_handle.has_value())
      return dir_handle.error();
    // Empty ObjectName + RootDirectory = reopen the same file object.
    us->Buffer = path_buf;
    us->Length = 0;
    us->MaximumLength = 0;
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->RootDirectory = dir_handle.value();
    oa->ObjectName = us;
    oa->Attributes = OBJ_CASE_INSENSITIVE;
    oa->SecurityDescriptor = nullptr;
    oa->SecurityQualityOfService = nullptr;
    return 0;
  }
  return resolve_at_path(dirfd, path, path_buf, buf_wchars, oa, us);
}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_H
