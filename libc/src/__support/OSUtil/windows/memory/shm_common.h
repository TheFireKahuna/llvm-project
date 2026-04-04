//===-- Shared helpers for shm_open/shm_unlink on Windows --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX shm names are mapped to files in libc's resolved temp root under
// llvm_shm\. This header
// provides the name validation and path construction shared by shm_open
// and shm_unlink.
//
// All functions return negative errno values on error (never set libc_errno).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SHM_COMMON_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SHM_COMMON_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/error_or.h"
#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/OSUtil/windows/temp_path.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace shm_common {

// Windows WIN_MAX_PATH equivalent — we define our own since we don't use <Windows.h>.
constexpr int WIN_MAX_PATH = 260;
constexpr int SHM_NAME_MAX = 255;
constexpr int SHM_PATH_MAX = WIN_MAX_PATH * 3 + SHM_NAME_MAX + 32;

// WCHAR buffer for temp/NT path conversions in shm helpers.
// get_temp_path_w output is capped at WIN_MAX_PATH (checked below), and
// to_nt_path adds a small prefix (\??\, ~4 WCHARs). Using MAX_NT_PATH_WCHARS
// (32768 = 64 KB per array) would blow the stack on worker threads whose
// initial commit is only a few KB — the 64 KB array jumps past the guard page,
// causing STATUS_STACK_OVERFLOW (0xC00000FD).
constexpr size_t SHM_WIDE_BUF = WIN_MAX_PATH + 16;

/// Build the full path for a POSIX shm name: <temp-root>\llvm_shm\<name>.
/// POSIX requires: starts with '/', no other '/', not "." or "..".
/// Returns the length (excluding NUL) on success, or negative errno on error.
LIBC_INLINE int build_path(const char *name, char *buf, int buf_size) {
  if (!name || name[0] != '/') {
    return -EINVAL;
  }

  const char *body = name + 1;
  if (body[0] == '\0' || body[0] == '.') {
    return -EINVAL;
  }

  for (const char *p = body; *p; ++p) {
    if (*p == '/') {
      return -EINVAL;
    }
  }

  // Resolve the libc-owned temp root.
  WCHAR temp_wide[SHM_WIDE_BUF];
  size_t temp_len = windows::get_temp_path_w(temp_wide, SHM_WIDE_BUF);
  if (temp_len == 0) {
    return -EIO;
  }
  if (temp_len > WIN_MAX_PATH) {
    return -ENAMETOOLONG;
  }

  // Convert to UTF-8.
  char temp_utf8[WIN_MAX_PATH * 3 + 1];
  int utf8_len = windows::wide_to_utf8_n(temp_wide, temp_len, temp_utf8,
                                         sizeof(temp_utf8));
  if (utf8_len < 0) {
    return -EINVAL;
  }

  // Assemble: temp_path + "llvm_shm\" + body
  // Reserve last byte for NUL terminator.
  cpp::StringStream ss(cpp::span<char>(buf, buf_size - 1));
  ss << cpp::string_view(temp_utf8, static_cast<size_t>(utf8_len))
     << "llvm_shm\\" << body;

  if (ss.overflow())
    return -ENAMETOOLONG;

  buf[ss.str().size()] = '\0';
  return static_cast<int>(ss.str().size());
}

/// Ensure the shm directory exists with hardened permissions.
/// Returns true if the directory is safe to use.
LIBC_INLINE bool ensure_dir(const char *shm_path) {
  const char *last_sep = nullptr;
  for (const char *p = shm_path; *p; ++p) {
    if (*p == '\\')
      last_sep = p;
  }
  if (!last_sep)
    return false;

  // Extract directory portion and convert to NT path.
  int dir_len = static_cast<int>(last_sep - shm_path);
  char dir_utf8[WIN_MAX_PATH * 3 + 1];
  if (dir_len >= static_cast<int>(sizeof(dir_utf8)))
    return false;
  __builtin_memcpy(dir_utf8, shm_path, static_cast<size_t>(dir_len));
  dir_utf8[dir_len] = '\0';

  WCHAR nt_path[SHM_WIDE_BUF];
  using LIBC_NAMESPACE::cpp::string_view;
  string_view dir_sv(dir_utf8);
  auto nt = to_nt_path(dir_sv, nt_path, SHM_WIDE_BUF);
  if (!nt.has_value())
    return false;
  size_t nt_len = nt.value();

  return windows_sec::ensure_secure_ipc_dir(nt_path, nt_len);
}

/// Build a path for memfd_create:
/// <temp-root>\llvm_shm\memfd_<name>_<pid>_<counter>.
/// The name is sanitized (no slashes). The pid and a process-local counter
/// ensure uniqueness even for identical names.
/// Returns the length (excluding NUL) on success, or negative errno on error.
LIBC_INLINE int build_memfd_path(const char *name, char *buf, int buf_size) {
  if (!name) {
    return -EINVAL;
  }

  WCHAR temp_wide[SHM_WIDE_BUF];
  size_t temp_len = windows::get_temp_path_w(temp_wide, SHM_WIDE_BUF);
  if (temp_len == 0) {
    return -EIO;
  }
  if (temp_len > WIN_MAX_PATH) {
    return -ENAMETOOLONG;
  }

  char temp_utf8[WIN_MAX_PATH * 3 + 1];
  int utf8_len = windows::wide_to_utf8_n(temp_wide, temp_len, temp_utf8,
                                         sizeof(temp_utf8));
  if (utf8_len < 0) {
    return -EINVAL;
  }

  // Monotonic counter for uniqueness within the process.
  static cpp::Atomic<unsigned> counter{0};
  unsigned seq = counter.fetch_add(1, cpp::MemoryOrder::RELAXED);
  DWORD pid = NtCurrentProcessId();

  // Assemble: temp_path + "llvm_shm\memfd_" + name + "_" + pid + "_" + seq
  // Reserve last byte for NUL terminator.
  cpp::StringStream ss(cpp::span<char>(buf, buf_size - 1));
  ss << cpp::string_view(temp_utf8, static_cast<size_t>(utf8_len))
     << "llvm_shm\\memfd_";

  // Sanitize name: replace non-alnum with '_'.
  for (const char *p = name; *p; ++p) {
    char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      c = '_';
    ss << c;
  }

  ss << '_' << static_cast<unsigned>(pid) << '_' << seq;

  if (ss.overflow())
    return -ENAMETOOLONG;

  buf[ss.str().size()] = '\0';
  return static_cast<int>(ss.str().size());
}

/// Unlink a shm/memfd path. Converts to wide string and calls NtDeleteFile
/// for true POSIX unlink semantics (name removed immediately, handles survive).
LIBC_INLINE void unlink_path(const char *path) {
  using LIBC_NAMESPACE::cpp::string_view;
  WCHAR wide[WIN_MAX_PATH + 1];
  string_view sv(path);
  int wlen = windows::utf8_to_wide(sv, wide, WIN_MAX_PATH);
  if (wlen <= 0)
    return;

  // Build NT path: \??\<path>
  WCHAR nt_path[WIN_MAX_PATH + 8];
  nt_path[0] = u'\\';
  nt_path[1] = u'?';
  nt_path[2] = u'?';
  nt_path[3] = u'\\';
  for (int i = 0; i < wlen && i + 4 < WIN_MAX_PATH + 7; ++i)
    nt_path[i + 4] = wide[i];

  windows::nt_wstring_view path_wsv(nt_path, static_cast<size_t>(wlen - 1 + 4));

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = nullptr;
  oa.ObjectName = path_wsv.unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  ::NtDeleteFile(&oa);
}

} // namespace shm_common
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SHM_COMMON_H
