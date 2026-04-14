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
  int pos = 0;
  auto append = [&](const char *s, int len) -> bool {
    if (pos + len >= buf_size) {
      return false;
    }
    for (int i = 0; i < len; ++i)
      buf[pos++] = s[i];
    return true;
  };

  if (!append(temp_utf8, utf8_len))
    return -ENAMETOOLONG;

  const char suffix[] = "llvm_shm\\";
  if (!append(suffix, sizeof(suffix) - 1))
    return -ENAMETOOLONG;

  int body_len = 0;
  for (const char *b = body; *b; ++b)
    ++body_len;
  if (!append(body, body_len))
    return -ENAMETOOLONG;

  buf[pos] = '\0';
  return pos;
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
  for (int i = 0; i < dir_len; ++i)
    dir_utf8[i] = shm_path[i];
  dir_utf8[dir_len] = '\0';

  WCHAR nt_path[SHM_WIDE_BUF];
  size_t nt_len = to_nt_path(dir_utf8, nt_path, SHM_WIDE_BUF);
  if (nt_len == 0)
    return false;

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
  int pos = 0;
  auto append_str = [&](const char *s) -> bool {
    while (*s) {
      if (pos + 1 >= buf_size) {
        return false;
      }
      buf[pos++] = *s++;
    }
    return true;
  };
  auto append_uint = [&](unsigned val) -> bool {
    char digits[16];
    int n = 0;
    if (val == 0)
      digits[n++] = '0';
    else
      while (val > 0) {
        digits[n++] = '0' + static_cast<char>(val % 10);
        val /= 10;
      }
    for (int i = n - 1; i >= 0; --i) {
      if (pos + 1 >= buf_size) {
        return false;
      }
      buf[pos++] = digits[i];
    }
    return true;
  };

  // Write temp_utf8 manually to respect utf8_len (no NUL terminator).
  for (int i = 0; i < utf8_len && pos + 1 < buf_size; ++i)
    buf[pos++] = temp_utf8[i];

  if (!append_str("llvm_shm\\memfd_"))
    return -ENAMETOOLONG;

  // Sanitize name: replace non-alnum with '_'.
  for (const char *p = name; *p; ++p) {
    char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      c = '_';
    if (pos + 1 >= buf_size) {
      return -ENAMETOOLONG;
    }
    buf[pos++] = c;
  }

  if (!append_str("_"))
    return -ENAMETOOLONG;
  if (!append_uint(static_cast<unsigned>(pid)))
    return -ENAMETOOLONG;
  if (!append_str("_"))
    return -ENAMETOOLONG;
  if (!append_uint(seq))
    return -ENAMETOOLONG;

  buf[pos] = '\0';
  return pos;
}

/// Unlink a shm/memfd path. Converts to wide string and calls NtDeleteFile
/// for true POSIX unlink semantics (name removed immediately, handles survive).
LIBC_INLINE void unlink_path(const char *path) {
  WCHAR wide[WIN_MAX_PATH + 1];
  int wlen = windows::utf8_to_wide(path, wide, WIN_MAX_PATH);
  if (wlen <= 0)
    return;

  // Build NT path: \??\<path>
  WCHAR nt_path[WIN_MAX_PATH + 8];
  nt_path[0] = L'\\';
  nt_path[1] = L'?';
  nt_path[2] = L'?';
  nt_path[3] = L'\\';
  for (int i = 0; i < wlen && i + 4 < WIN_MAX_PATH + 7; ++i)
    nt_path[i + 4] = wide[i];

  UNICODE_STRING us;
  us.Length = static_cast<USHORT>((wlen - 1 + 4) * sizeof(WCHAR));
  us.MaximumLength = static_cast<USHORT>((wlen + 4) * sizeof(WCHAR));
  us.Buffer = nt_path;

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = nullptr;
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  ::NtDeleteFile(&oa);
}

} // namespace shm_common
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_SHM_COMMON_H
