//===-- passwd database operations for Windows (NT-POSIX) -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements getpwuid/getpwnam by querying the Windows ProfileList registry:
//
//   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\<SID>
//     ProfileImagePath = "C:\Users\<username>"
//
// For the current user, the primary group comes from the process token. For
// other users, pw_gid defaults to pw_uid (matching Cygwin/WSL convention for
// local accounts without explicit group mapping).
//
// pw_shell is "/bin/sh" and pw_passwd is "*" (standard POSIX defaults).
// pw_gecos is empty. pw_dir uses forward slashes (POSIX normalization).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/passwd_ops.h"

#include "hdr/types/gid_t.h"
#include "hdr/types/uid_t.h"
#include "include/llvm-libc-types/struct_passwd.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string_types.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

// Maximum SID string length: "S-1-281474976710655-4294967295-..." with up to
// 15 sub-authorities ≈ 190 chars. 256 is generous.
constexpr size_t MAX_SID_STRING = 256;

// ProfileList registry key path.
constexpr WCHAR PROFILE_LIST_KEY[] =
    u"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT"
    u"\\CurrentVersion\\ProfileList";

// Static strings embedded into the caller's buffer.
constexpr char PW_PASSWD[] = "*";
constexpr char PW_GECOS[] = "";
constexpr char PW_SHELL[] = "/bin/sh";

// Helper: open the ProfileList registry key.
// Returns STATUS_SUCCESS on success, NTSTATUS error otherwise.
NTSTATUS open_profile_list(HANDLE *key_out) {
  UNICODE_STRING key_name;
  key_name.Buffer = const_cast<WCHAR *>(PROFILE_LIST_KEY);
  key_name.Length = sizeof(PROFILE_LIST_KEY) - sizeof(WCHAR);
  key_name.MaximumLength = sizeof(PROFILE_LIST_KEY);

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = nullptr;
  oa.ObjectName = &key_name;
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  return ::NtOpenKeyEx(key_out, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &oa,
                       0);
}

// Helper: open a subkey under an already-opened parent key.
NTSTATUS open_subkey(HANDLE parent, const WCHAR *name, ULONG name_bytes,
                     HANDLE *key_out) {
  UNICODE_STRING key_name;
  key_name.Buffer = const_cast<WCHAR *>(name);
  key_name.Length = static_cast<USHORT>(name_bytes);
  key_name.MaximumLength = static_cast<USHORT>(name_bytes + sizeof(WCHAR));

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = parent;
  oa.ObjectName = &key_name;
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  return ::NtOpenKeyEx(key_out, KEY_QUERY_VALUE, &oa, 0);
}

// Helper: query ProfileImagePath from an opened profile subkey.
// Writes the wide profile path into dst (up to dst_cap WCHARs).
// Returns the number of WCHARs written (excluding NUL), or 0 on failure.
size_t query_profile_path(HANDLE subkey, WCHAR *dst, size_t dst_cap) {
  WCHAR value_name_buf[] = u"ProfileImagePath";
  UNICODE_STRING value_name;
  value_name.Buffer = value_name_buf;
  value_name.Length = sizeof(value_name_buf) - sizeof(WCHAR);
  value_name.MaximumLength = sizeof(value_name_buf);

  // Buffer for KEY_VALUE_PARTIAL_INFORMATION + up to 520 bytes of path data.
  alignas(KEY_VALUE_PARTIAL_INFORMATION) char
      buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 520];
  ULONG result_len = 0;

  NTSTATUS status = ::NtQueryValueKey(subkey, &value_name,
                                      KeyValuePartialInformation, buf,
                                      sizeof(buf), &result_len);
  // Reject both failures and STATUS_BUFFER_OVERFLOW (0x80000005), which
  // passes NT_SUCCESS but delivers a truncated write with DataLength set
  // to the true (untruncated) size — using that would cause an OOB read.
  if (!NT_SUCCESS(status) || status == STATUS_BUFFER_OVERFLOW)
    return 0;

  auto *info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION *>(buf);
  if ((info->Type != REG_SZ && info->Type != REG_EXPAND_SZ) ||
      info->DataLength == 0)
    return 0;

  // Clamp DataLength to what actually fits in our buffer as a defense
  // against any future NT status edge cases.
  ULONG max_data =
      static_cast<ULONG>(sizeof(buf)) -
      static_cast<ULONG>(offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data));
  auto *wide_data = reinterpret_cast<const WCHAR *>(info->Data);
  ULONG wide_bytes = info->DataLength;
  if (wide_bytes > max_data)
    wide_bytes = max_data;

  // Strip trailing NUL if present.
  if (wide_bytes >= sizeof(WCHAR) &&
      wide_data[wide_bytes / sizeof(WCHAR) - 1] == u'\0')
    wide_bytes -= sizeof(WCHAR);

  size_t wide_len = wide_bytes / sizeof(WCHAR);
  if (wide_len == 0 || wide_len >= dst_cap)
    return 0;

  for (size_t i = 0; i < wide_len; ++i)
    dst[i] = wide_data[i];
  dst[wide_len] = u'\0';
  return wide_len;
}

// Extract the username from a profile path by finding the last path separator.
// "C:\Users\john" -> pointer to "john", length 4.
// Returns pointer into the input buffer; sets *out_len.
const WCHAR *extract_username(const WCHAR *path, size_t path_len,
                              size_t *out_len) {
  size_t last_sep = 0;
  bool found = false;
  for (size_t i = 0; i < path_len; ++i) {
    if (path[i] == u'\\' || path[i] == u'/') {
      last_sep = i;
      found = true;
    }
  }
  if (!found) {
    *out_len = path_len;
    return path;
  }
  *out_len = path_len - last_sep - 1;
  return path + last_sep + 1;
}

// Parse the RID (last number after the last '-') from a SID string.
// "S-1-5-21-xxx-yyy-zzz-1001" -> 1001
// Returns the uid, or (uid_t)-1 on parse failure.
uid_t parse_rid_from_sid_string(const WCHAR *sid_str, size_t sid_len) {
  // Find the last '-'.
  size_t last_dash = 0;
  bool found = false;
  for (size_t i = 0; i < sid_len; ++i) {
    if (sid_str[i] == u'-') {
      last_dash = i;
      found = true;
    }
  }
  if (!found || last_dash + 1 >= sid_len)
    return static_cast<uid_t>(-1);

  // Parse the decimal number after the last dash.
  uid_t result = 0;
  for (size_t i = last_dash + 1; i < sid_len; ++i) {
    if (sid_str[i] < u'0' || sid_str[i] > u'9')
      return static_cast<uid_t>(-1);
    uid_t digit = static_cast<uid_t>(sid_str[i] - u'0');
    if (result > (static_cast<uid_t>(-1) - digit) / 10)
      return static_cast<uid_t>(-1); // overflow
    result = result * 10 + digit;
  }
  return result;
}

// Build a SID string for a given uid into dst.
// Tries the domain-relative SID first (via id_to_sid), then falls back to
// well-known authority SIDs (S-1-5-<uid>) for service accounts.
// Returns the number of WCHARs written (excluding NUL), or 0 on failure.
size_t uid_to_sid_string(uid_t uid, WCHAR *dst, size_t dst_cap) {
  alignas(8) UCHAR sid_buf[MAX_SID_SIZE];
  auto *sid = reinterpret_cast<SID *>(sid_buf);

  if (id_to_sid(static_cast<unsigned long>(uid), sid)) {
    UNICODE_STRING us = {};
    NTSTATUS status = ::RtlConvertSidToUnicodeString(&us, sid, 1);
    if (NT_SUCCESS(status)) {
      size_t len = us.Length / sizeof(WCHAR);
      if (len < dst_cap) {
        for (size_t i = 0; i < len; ++i)
          dst[i] = us.Buffer[i];
        dst[len] = u'\0';
        ::RtlFreeUnicodeString(&us);
        return len;
      }
      ::RtlFreeUnicodeString(&us);
    }
  }
  return 0;
}

// Build a well-known SID string "S-1-5-<uid>" for service accounts.
// These are non-domain-relative SIDs (SYSTEM=18, LocalService=19, etc.).
size_t uid_to_wellknown_sid_string(uid_t uid, WCHAR *dst, size_t dst_cap) {
  // Build S-1-5-<uid> manually.
  constexpr WCHAR prefix[] = u"S-1-5-";
  constexpr size_t prefix_len = sizeof(prefix) / sizeof(WCHAR) - 1;

  // Convert uid to decimal digits.
  WCHAR digits[12];
  size_t ndigits = 0;
  uid_t val = uid;
  if (val == 0) {
    digits[ndigits++] = u'0';
  } else {
    while (val > 0) {
      digits[ndigits++] = static_cast<WCHAR>(u'0' + (val % 10));
      val /= 10;
    }
  }

  size_t total = prefix_len + ndigits;
  if (total >= dst_cap)
    return 0;

  for (size_t i = 0; i < prefix_len; ++i)
    dst[i] = prefix[i];
  for (size_t i = 0; i < ndigits; ++i)
    dst[prefix_len + i] = digits[ndigits - 1 - i];
  dst[total] = u'\0';
  return total;
}

// Look up a user by SID string in the ProfileList registry.
// On success, writes profile path to path_buf and returns the path length.
// On failure, returns 0.
size_t lookup_profile_by_sid_string(HANDLE profile_list_key,
                                    const WCHAR *sid_str, size_t sid_len,
                                    WCHAR *path_buf, size_t path_cap) {
  HANDLE subkey = nullptr;
  NTSTATUS status = open_subkey(profile_list_key, sid_str,
                                static_cast<ULONG>(sid_len * sizeof(WCHAR)),
                                &subkey);
  if (!NT_SUCCESS(status))
    return 0;

  size_t result = query_profile_path(subkey, path_buf, path_cap);
  ::NtClose(subkey);
  return result;
}

// Get the current user's primary GID from the process token.
gid_t get_current_user_gid() {
  return g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED);
}

// Get the current user's UID from the process token.
uid_t get_current_user_uid() {
  return g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
}

// A small string builder that packs multiple NUL-terminated strings into a
// flat char buffer and returns pointers into it for struct passwd fields.
struct BufWriter {
  char *buf;
  size_t cap;
  size_t pos;

  BufWriter(char *b, size_t c) : buf(b), cap(c), pos(0) {}

  // Write a C string literal. Returns pointer to the copy, or nullptr if
  // the buffer is too small.
  char *put_str(const char *s) {
    char *start = buf + pos;
    size_t i = 0;
    while (s[i] != '\0') {
      if (pos >= cap)
        return nullptr;
      buf[pos++] = s[i++];
    }
    if (pos >= cap)
      return nullptr;
    buf[pos++] = '\0';
    return start;
  }

  // Write a wide string, converting to UTF-8. Normalizes backslashes to
  // forward slashes when normalize_path is true.
  // Returns pointer to the UTF-8 copy, or nullptr on failure.
  char *put_wide(const WCHAR *src, size_t src_len, bool normalize_path) {
    if (pos >= cap)
      return nullptr;

    char *start = buf + pos;
    size_t remaining = cap - pos;

    // Need at least 1 byte for NUL terminator.
    if (remaining < 2)
      return nullptr;

    int utf8_bytes = windows::wide_to_utf8_n(src, src_len, start,
                                             remaining - 1);
    if (utf8_bytes < 0)
      return nullptr;

    if (normalize_path) {
      for (int i = 0; i < utf8_bytes; ++i) {
        if (start[i] == '\\')
          start[i] = '/';
      }
    }

    start[utf8_bytes] = '\0';
    pos += static_cast<size_t>(utf8_bytes) + 1;
    return start;
  }
};

// Core: fill a passwd struct from a profile path and uid/gid.
int fill_passwd_from_profile(uid_t uid, gid_t gid, const WCHAR *profile_path,
                             size_t path_len, struct passwd *pwd, char *buf,
                             size_t buflen) {
  size_t name_len = 0;
  const WCHAR *name = extract_username(profile_path, path_len, &name_len);

  BufWriter w(buf, buflen);

  // pw_name: username extracted from profile path.
  pwd->pw_name = w.put_wide(name, name_len, false);
  if (!pwd->pw_name)
    return ERANGE;

  // pw_passwd: always "*".
  pwd->pw_passwd = w.put_str(PW_PASSWD);
  if (!pwd->pw_passwd)
    return ERANGE;

  // pw_gecos: empty string.
  pwd->pw_gecos = w.put_str(PW_GECOS);
  if (!pwd->pw_gecos)
    return ERANGE;

  // pw_dir: full profile path with forward slashes.
  pwd->pw_dir = w.put_wide(profile_path, path_len, true);
  if (!pwd->pw_dir)
    return ERANGE;

  // pw_shell: "/bin/sh".
  pwd->pw_shell = w.put_str(PW_SHELL);
  if (!pwd->pw_shell)
    return ERANGE;

  pwd->pw_uid = uid;
  pwd->pw_gid = gid;
  return 0;
}

// Case-insensitive comparison of a wide string against a UTF-8 string.
// Converts the wide string to UTF-8 first, then compares byte-by-byte with
// ASCII case folding. This correctly handles multi-byte UTF-8 sequences.
//
// Windows usernames are case-insensitive at the OS level, so case-insensitive
// matching is the correct behavior for getpwnam on this platform. POSIX
// requires case-sensitive matching, but that would make lookups fail for
// users whose profile path casing differs from the input — a common scenario
// on Windows where "John" and "john" refer to the same account.
bool wide_eq_utf8_icase(const WCHAR *wide, size_t wide_len, const char *utf8) {
  // Convert wide string to UTF-8 into a stack buffer.
  char converted[512];
  int conv_len = windows::wide_to_utf8_n(wide, wide_len, converted,
                                         sizeof(converted) - 1);
  if (conv_len < 0)
    return false;
  converted[conv_len] = '\0';

  // Case-insensitive byte comparison (ASCII range).
  const char *a = converted;
  const char *b = utf8;
  while (*a && *b) {
    unsigned char ca = static_cast<unsigned char>(*a);
    unsigned char cb = static_cast<unsigned char>(*b);
    if (ca >= 'A' && ca <= 'Z')
      ca += 32;
    if (cb >= 'A' && cb <= 'Z')
      cb += 32;
    if (ca != cb)
      return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

} // namespace

//===----------------------------------------------------------------------===//
// fill_passwd_uid — look up user by uid
//===----------------------------------------------------------------------===//

int fill_passwd_uid(uid_t uid, struct passwd *pwd, char *buf, size_t buflen) {
  HANDLE profile_list = nullptr;
  NTSTATUS status = open_profile_list(&profile_list);
  if (!NT_SUCCESS(status))
    return ENOENT;

  WCHAR sid_str[MAX_SID_STRING];
  WCHAR path_buf[260];
  size_t path_len = 0;

  // Try 1: domain-relative SID (covers regular user accounts).
  size_t sid_len = uid_to_sid_string(uid, sid_str, MAX_SID_STRING);
  if (sid_len > 0)
    path_len = lookup_profile_by_sid_string(profile_list, sid_str, sid_len,
                                            path_buf, 260);

  // Try 2: well-known SID S-1-5-<uid> (covers SYSTEM, LocalService, etc.).
  if (path_len == 0) {
    sid_len = uid_to_wellknown_sid_string(uid, sid_str, MAX_SID_STRING);
    if (sid_len > 0)
      path_len = lookup_profile_by_sid_string(profile_list, sid_str, sid_len,
                                              path_buf, 260);
  }

  // Try 3: enumerate all subkeys and match by RID.
  // This handles cases where the SID structure doesn't match our assumptions
  // (e.g., different domain prefix, Azure AD accounts, etc.).
  if (path_len == 0) {
    alignas(KEY_BASIC_INFORMATION) char
        key_buf[sizeof(KEY_BASIC_INFORMATION) + MAX_SID_STRING * sizeof(WCHAR)];
    ULONG result_len = 0;

    for (ULONG index = 0;; ++index) {
      status =
          ::NtEnumerateKey(profile_list, index, KeyBasicInformation, key_buf,
                           sizeof(key_buf), &result_len);
      if (!NT_SUCCESS(status))
        break;
      // STATUS_BUFFER_OVERFLOW passes NT_SUCCESS but truncates the name —
      // skip this entry rather than reading garbage.
      if (status == STATUS_BUFFER_OVERFLOW)
        continue;

      auto *info = reinterpret_cast<KEY_BASIC_INFORMATION *>(key_buf);
      size_t name_wchars = info->NameLength / sizeof(WCHAR);
      // Clamp to what actually fits in our buffer.
      size_t max_name_wchars =
          (sizeof(key_buf) - offsetof(KEY_BASIC_INFORMATION, Name)) /
          sizeof(WCHAR);
      if (name_wchars > max_name_wchars)
        name_wchars = max_name_wchars;

      uid_t candidate_uid =
          parse_rid_from_sid_string(info->Name, name_wchars);
      if (candidate_uid == uid) {
        path_len = lookup_profile_by_sid_string(
            profile_list, info->Name, name_wchars, path_buf, 260);
        break;
      }
    }
  }

  ::NtClose(profile_list);

  if (path_len == 0)
    return ENOENT;

  // Use the process token's GID for the current user; default to uid for
  // other users (consistent with Cygwin/WSL for local accounts).
  gid_t gid =
      (uid == get_current_user_uid()) ? get_current_user_gid()
                                      : static_cast<gid_t>(uid);

  return fill_passwd_from_profile(uid, gid, path_buf, path_len, pwd, buf,
                                  buflen);
}

//===----------------------------------------------------------------------===//
// fill_passwd_name — look up user by name
//===----------------------------------------------------------------------===//

int fill_passwd_name(const char *name, struct passwd *pwd, char *buf,
                     size_t buflen) {
  if (!name)
    return EINVAL;

  HANDLE profile_list = nullptr;
  NTSTATUS status = open_profile_list(&profile_list);
  if (!NT_SUCCESS(status))
    return ENOENT;

  WCHAR path_buf[260];
  size_t found_path_len = 0;
  uid_t found_uid = static_cast<uid_t>(-1);

  // Enumerate all ProfileList subkeys, read each ProfileImagePath, and
  // compare the extracted username against the target name.
  alignas(KEY_BASIC_INFORMATION) char
      key_buf[sizeof(KEY_BASIC_INFORMATION) + MAX_SID_STRING * sizeof(WCHAR)];
  ULONG result_len = 0;

  for (ULONG index = 0;; ++index) {
    status =
        ::NtEnumerateKey(profile_list, index, KeyBasicInformation, key_buf,
                         sizeof(key_buf), &result_len);
    if (!NT_SUCCESS(status))
      break;

    auto *info = reinterpret_cast<KEY_BASIC_INFORMATION *>(key_buf);
    size_t sid_wchars = info->NameLength / sizeof(WCHAR);

    // Open this SID's subkey and read ProfileImagePath.
    size_t path_len = lookup_profile_by_sid_string(
        profile_list, info->Name, sid_wchars, path_buf, 260);
    if (path_len == 0)
      continue;

    // Extract username from profile path and compare.
    size_t uname_len = 0;
    const WCHAR *uname = extract_username(path_buf, path_len, &uname_len);
    if (wide_eq_utf8_icase(uname, uname_len, name)) {
      found_uid = parse_rid_from_sid_string(info->Name, sid_wchars);
      found_path_len = path_len;
      break;
    }
  }

  ::NtClose(profile_list);

  if (found_path_len == 0 || found_uid == static_cast<uid_t>(-1))
    return ENOENT;

  gid_t gid =
      (found_uid == get_current_user_uid()) ? get_current_user_gid()
                                            : static_cast<gid_t>(found_uid);

  return fill_passwd_from_profile(found_uid, gid, path_buf, found_path_len,
                                  pwd, buf, buflen);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
