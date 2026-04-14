//===-- Windows temp-path resolution -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Resolves the process temp directory without going through the Win32
// GetTempPath* layer. The policy is intentionally simple and libc-owned:
//   1. TMP
//   2. TEMP
//   3. LOCALAPPDATA + "\\Temp"
//   4. <NtSystemRoot> + "\\Temp"
//
// The result is always a DOS-style wide path with a trailing backslash.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEMP_PATH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEMP_PATH_H

#include "hdr/types/size_t.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

LIBC_INLINE WCHAR ascii_uppercase_w(WCHAR ch) {
  return (ch >= u'a' && ch <= u'z') ? static_cast<WCHAR>(ch - (u'a' - u'A'))
                                    : ch;
}

LIBC_INLINE bool ascii_equals_ignore_case_w(const WCHAR *lhs, const WCHAR *rhs,
                                            size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (ascii_uppercase_w(lhs[i]) != ascii_uppercase_w(rhs[i]))
      return false;
  }
  return true;
}

LIBC_INLINE size_t wide_nul_terminated_length(const WCHAR *text) {
  size_t len = 0;
  while (text && text[len] != u'\0')
    ++len;
  return len;
}

LIBC_INLINE size_t copy_normalized_path_with_trailing_backslash(
    const WCHAR *src, size_t src_len, WCHAR *dst, size_t dst_cap) {
  if (!dst || dst_cap == 0)
    return 0;

  bool needs_backslash =
      src_len == 0 ||
      (src[src_len - 1] != u'\\' && src[src_len - 1] != u'/');
  size_t required = src_len + (needs_backslash ? 1 : 0);
  if (required + 1 > dst_cap)
    return 0;

  for (size_t i = 0; i < src_len; ++i)
    dst[i] = (src[i] == u'/') ? u'\\' : src[i];
  if (needs_backslash)
    dst[src_len++] = u'\\';
  dst[src_len] = u'\0';
  return src_len;
}

LIBC_INLINE const WCHAR *find_env_value_case_insensitive(
    const WCHAR *env_block, const WCHAR *name, size_t name_len,
    size_t *value_len_out) {
  if (value_len_out)
    *value_len_out = 0;
  if (!env_block || !name || name_len == 0)
    return nullptr;

  for (const WCHAR *entry = env_block; *entry != u'\0';) {
    const WCHAR *cursor = entry;
    while (*cursor != u'\0' && *cursor != u'=')
      ++cursor;

    if (*cursor == u'=' && static_cast<size_t>(cursor - entry) == name_len &&
        ascii_equals_ignore_case_w(entry, name, name_len)) {
      const WCHAR *value = cursor + 1;
      size_t value_len = wide_nul_terminated_length(value);
      if (value_len_out)
        *value_len_out = value_len;
      return value_len == 0 ? nullptr : value;
    }

    while (*cursor != u'\0')
      ++cursor;
    entry = cursor + 1;
  }

  return nullptr;
}

LIBC_INLINE size_t get_temp_path_from_env_block(const WCHAR *env_block,
                                                const WCHAR *system_root,
                                                WCHAR *dst, size_t dst_cap) {
  static constexpr WCHAR TMP_NAME[] = {u'T', u'M', u'P', u'\0'};
  static constexpr WCHAR TEMP_NAME[] = {u'T', u'E', u'M', u'P', u'\0'};
  static constexpr WCHAR LOCALAPPDATA_NAME[] = {
      u'L', u'O', u'C', u'A', u'L', u'A', u'P', u'P', u'D', u'A', u'T', u'A',
      u'\0'};
  static constexpr WCHAR TEMP_SUFFIX[] = {
      u'\\', u'T', u'e', u'm', u'p', u'\0'};

  size_t value_len = 0;
  if (const WCHAR *value =
          find_env_value_case_insensitive(env_block, TMP_NAME, 3, &value_len)) {
    if (size_t len =
            copy_normalized_path_with_trailing_backslash(value, value_len, dst,
                                                         dst_cap)) {
      return len;
    }
  }

  if (const WCHAR *value = find_env_value_case_insensitive(env_block, TEMP_NAME,
                                                           4, &value_len)) {
    if (size_t len =
            copy_normalized_path_with_trailing_backslash(value, value_len, dst,
                                                         dst_cap)) {
      return len;
    }
  }

  if (const WCHAR *value = find_env_value_case_insensitive(
          env_block, LOCALAPPDATA_NAME, 12, &value_len)) {
    size_t suffix_len = 5; // "\\Temp"
    if (value_len + suffix_len + 2 <= dst_cap) {
      for (size_t i = 0; i < value_len; ++i)
        dst[i] = (value[i] == u'/') ? u'\\' : value[i];
      bool needs_sep =
          value_len == 0 ||
          (dst[value_len - 1] != u'\\' && dst[value_len - 1] != u'/');
      size_t pos = value_len;
      if (needs_sep)
        dst[pos++] = u'\\';
      for (size_t i = 1; TEMP_SUFFIX[i] != u'\0'; ++i)
        dst[pos++] = TEMP_SUFFIX[i];
      dst[pos++] = u'\\';
      dst[pos] = u'\0';
      return pos;
    }
  }

  size_t system_root_len = wide_nul_terminated_length(system_root);
  if (system_root_len == 0)
    return 0;
  if (system_root_len + 6 + 1 > dst_cap)
    return 0;

  for (size_t i = 0; i < system_root_len; ++i)
    dst[i] = (system_root[i] == u'/') ? u'\\' : system_root[i];
  size_t pos = system_root_len;
  if (dst[pos - 1] != u'\\')
    dst[pos++] = u'\\';
  dst[pos++] = u'T';
  dst[pos++] = u'e';
  dst[pos++] = u'm';
  dst[pos++] = u'p';
  dst[pos++] = u'\\';
  dst[pos] = u'\0';
  return pos;
}

LIBC_INLINE size_t get_temp_path_w(WCHAR *dst, size_t dst_cap) {
  if (!dst || dst_cap == 0)
    return 0;

  RTL_USER_PROCESS_PARAMETERS *params = NtCurrentPeb()->ProcessParameters;
  const WCHAR *env_block =
      params ? static_cast<const WCHAR *>(params->Environment) : nullptr;
  const auto *sud = windows_util::shared_user_data();
  return get_temp_path_from_env_block(env_block, sud->NtSystemRoot,
                                      dst, dst_cap);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEMP_PATH_H
