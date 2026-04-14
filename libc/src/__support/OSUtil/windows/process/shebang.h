//===-- Shebang (#!) script parser --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Parses the first line of a file for "#!interpreter [arg]".
// Shared between execve (exec_ops.cpp) and posix_spawn (spawn_ops.cpp).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SHEBANG_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SHEBANG_H

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Maximum shebang line length (Linux uses 256; POSIX doesn't specify).
inline constexpr int SHEBANG_MAX_LINE = 256;

// Maximum interpreter/arg path length.
inline constexpr int SHEBANG_MAX_PATH = 1024;

// Maximum recursion depth for nested shebangs (e.g., script -> awk -> ...).
inline constexpr int SHEBANG_MAX_DEPTH = 4;

// Parse the first line of a file looking for "#!interpreter [arg]".
// nt_path/nt_path_len: NT-format path to the file (e.g., \??\C:\foo\bar.sh).
// interp_buf: receives the interpreter path (null-terminated).
// interp_len: receives the interpreter path length (excluding null).
// arg_buf: receives the optional single argument (null-terminated).
// arg_len: receives the argument length (0 if none).
// Returns 0 on success, -ENOEXEC if no shebang found.
LIBC_INLINE int parse_shebang(const WCHAR *nt_path, size_t nt_path_len,
                              char *interp_buf, int *interp_len,
                              char *arg_buf, int *arg_len) {
  // Open the file for reading.
  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(nt_path_len * sizeof(WCHAR));
  us.MaximumLength = us.Length + sizeof(WCHAR);
  us.Buffer = const_cast<WCHAR *>(nt_path);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  HANDLE file = nullptr;
  NTSTATUS st = ::NtOpenFile(
      &file, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(st))
    return -ENOEXEC;

  // Read the first SHEBANG_MAX_LINE bytes.
  char line[SHEBANG_MAX_LINE];
  iosb = {};
  st = ::NtReadFile(file, nullptr, nullptr, nullptr, &iosb, line,
                    SHEBANG_MAX_LINE, nullptr, nullptr);
  ::NtClose(file);

  if (!NT_SUCCESS(st) && st != STATUS_END_OF_FILE)
    return -ENOEXEC;

  int bytes_read = static_cast<int>(iosb.Information);
  if (bytes_read < 2 || line[0] != '#' || line[1] != '!')
    return -ENOEXEC;

  // Find end of line (newline or EOF).
  int eol = 2;
  while (eol < bytes_read && line[eol] != '\n' && line[eol] != '\r')
    ++eol;

  // Skip whitespace after "#!".
  int pos = 2;
  while (pos < eol && (line[pos] == ' ' || line[pos] == '\t'))
    ++pos;

  if (pos >= eol)
    return -ENOEXEC; // No interpreter specified.

  // Extract interpreter path (first non-whitespace token).
  int istart = pos;
  while (pos < eol && line[pos] != ' ' && line[pos] != '\t')
    ++pos;
  int ilen = pos - istart;

  if (ilen == 0 || ilen >= SHEBANG_MAX_PATH)
    return -ENOEXEC;

  __builtin_memcpy(interp_buf, line + istart, ilen);
  interp_buf[ilen] = '\0';
  *interp_len = ilen;

  // Skip whitespace between interpreter and optional argument.
  while (pos < eol && (line[pos] == ' ' || line[pos] == '\t'))
    ++pos;

  // Extract optional single argument (POSIX: at most one argument).
  *arg_len = 0;
  if (pos < eol) {
    int astart = pos;
    // Trim trailing whitespace.
    int aend = eol;
    while (aend > astart &&
           (line[aend - 1] == ' ' || line[aend - 1] == '\t'))
      --aend;
    int alen = aend - astart;
    if (alen > 0 && alen < SHEBANG_MAX_PATH) {
      __builtin_memcpy(arg_buf, line + astart, alen);
      arg_buf[alen] = '\0';
      *arg_len = alen;
    }
  }

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SHEBANG_H
