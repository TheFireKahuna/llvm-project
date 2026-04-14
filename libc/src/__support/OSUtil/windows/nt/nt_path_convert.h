//===-- NT path conversion (compatibility wrapper) ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin compatibility wrapper around path_resolver.h. Existing callers that
// use to_nt_path() continue to work unchanged. New code should use
// resolve_path() / classify_path() directly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_CONVERT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_CONVERT_H

#include "src/__support/OSUtil/windows/nt/path_resolver.h"

// Convert a narrow (UTF-8) path to a wide NT path (\??\C:\...).
// Returns the length in WCHARs (excluding NUL), or 0 on failure.
//
// This is a thin wrapper around resolve_path(). It handles all path types:
// - POSIX /dev/* virtual paths -> NT device object paths
// - POSIX /tmp paths -> system temp directory
// - POSIX absolute paths -> system drive root
// - DOS absolute/relative, UNC, verbatim paths -> NT \??\ paths
//
// Virtual-only paths (DevPty, DevFd) cannot be expressed as NT paths;
// for those, use resolve_path() directly.
LIBC_INLINE size_t to_nt_path(const char *path, WCHAR *buf,
                               size_t max_wchars) {
  ResolvedPath rp = resolve_path(path, buf, max_wchars);
  if (rp.error || rp.nt_len == 0)
    return 0;
  return rp.nt_len;
}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_CONVERT_H
