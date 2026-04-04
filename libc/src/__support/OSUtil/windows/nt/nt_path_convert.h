//===-- NT path conversion (compatibility wrapper) ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Public path resolution API for the NT-POSIX libc. Converts a string_view
// path to a wide NT path via the internal path_detail::resolve_path().
// For virtual-only paths (DevPty, DevFd) or PathKind dispatch,
// use path_detail::resolve_path() directly (see openat in fcntl.cpp).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_CONVERT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_CONVERT_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/path_resolver.h"
#include "src/__support/error_or.h"

// Convert a narrow (UTF-8) path to a wide NT path (\??\C:\...).
// Returns ErrorOr<size_t>: the length in WCHARs (excluding NUL) on success,
// or an Error with the errno from resolve_path() (or EINVAL for virtual-only
// paths that have no NT representation).
//
// This is a thin wrapper around resolve_path(). It handles all path types:
// - POSIX /dev/* virtual paths -> NT device object paths
// - POSIX /tmp paths -> system temp directory
// - POSIX absolute paths -> system drive root
// - DOS absolute/relative, UNC, verbatim paths -> NT \??\ paths
//
// Virtual-only paths (DevPty, DevFd) cannot be expressed as NT paths;
// for those, use resolve_path() directly.
//
// Null checking is the caller's responsibility.
LIBC_INLINE LIBC_NAMESPACE::ErrorOr<size_t>
to_nt_path(LIBC_NAMESPACE::cpp::string_view path, WCHAR *buf,
           size_t max_wchars) {
  ResolvedPath rp = path_detail::resolve_path(path, buf, max_wchars);
  if (rp.error)
    return LIBC_NAMESPACE::Error(rp.error);
  if (rp.nt_len == 0)
    return LIBC_NAMESPACE::Error(EINVAL); // virtual-only path (DevPty, DevFd)
  return rp.nt_len;
}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_CONVERT_H
