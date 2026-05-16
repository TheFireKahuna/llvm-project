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
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
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
  windows::nt_wstring_view path_wsv(nt_path, nt_path_len);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = path_wsv.unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  windows::ScopedNtHandle file;
  NTSTATUS st = ::NtOpenFile(
      file.put(), FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(st))
    return -ENOEXEC;

  // Read the first SHEBANG_MAX_LINE bytes.
  char line[SHEBANG_MAX_LINE];
  iosb = {};
  st = ::NtReadFile(file.get(), nullptr, nullptr, nullptr, &iosb, line,
                    SHEBANG_MAX_LINE, nullptr, nullptr);
  file.reset();

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

// Callback type for try_shebang_retry: recursively launch the interpreter.
using ShebangRetryCallback = intptr_t (*)(const char *interp,
                                          char *const *new_argv,
                                          int new_depth, void *ctx);

// Shared shebang retry with depth tracking.
// Call when a launch attempt returned ENOEXEC. Parses the shebang, builds
// new argv, and calls recurse_fn with the interpreter path. ctx is passed
// through opaquely to the callback.
// Returns the callback result, or -ENOEXEC if no shebang was found,
// or -ELOOP if the depth limit is exceeded.
LIBC_INLINE intptr_t try_shebang_retry(const WCHAR *nt_path,
                                       size_t nt_path_len,
                                       const char *script_path,
                                       char *const *original_argv, int depth,
                                       ShebangRetryCallback recurse_fn,
                                       void *ctx) {
  if (depth >= SHEBANG_MAX_DEPTH)
    return -ELOOP;

  internal::ScratchAlloc<char> interp_s(SHEBANG_MAX_PATH);
  internal::ScratchAlloc<char> interp_arg_s(SHEBANG_MAX_PATH);
  if (!interp_s || !interp_arg_s)
    return -ENOEXEC; // OOM during shebang — treat as non-script
  char *interp = interp_s.data();
  char *interp_arg = interp_arg_s.data();
  int interp_len = 0, arg_len = 0;

  if (parse_shebang(nt_path, nt_path_len, interp, &interp_len, interp_arg,
                    &arg_len) != 0)
    return -ENOEXEC; // No shebang found.

  // Build new argv: [interpreter, optional_arg, script_path, argv[1:]]
  int orig_argc = 0;
  if (original_argv) {
    for (int i = 0; original_argv[i]; ++i)
      ++orig_argc;
  }

  int new_argc =
      1 + (arg_len > 0 ? 1 : 0) + 1 + (orig_argc > 1 ? orig_argc - 1 : 0);
  internal::ScratchAlloc<char *> new_argv_s(new_argc + 1);
  if (!new_argv_s)
    return -E2BIG;
  char **new_argv = new_argv_s.data();
  int pos = 0;
  new_argv[pos++] = interp;
  if (arg_len > 0)
    new_argv[pos++] = interp_arg;
  new_argv[pos++] = const_cast<char *>(script_path);
  if (original_argv) {
    for (int i = 1; i < orig_argc; ++i)
      new_argv[pos++] = original_argv[i];
  }
  new_argv[pos] = nullptr;

  return recurse_fn(interp, new_argv, depth + 1, ctx);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SHEBANG_H
