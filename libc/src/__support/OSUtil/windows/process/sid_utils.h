//===-- SID ↔ uid/gid conversion utilities ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared helpers for converting between Windows SIDs and POSIX numeric IDs.
//
// Mapping convention: the last sub-authority of a domain-relative SID is the
// POSIX numeric ID. For example, S-1-5-21-<domain>-1001 → uid_t 1001.
// This is the same convention used by Cygwin and WSL.
//
// These are LIBC_INLINE so they can be used from both process_identity.cpp
// (early init, before pools) and identity_ops.cpp (runtime set* operations)
// without creating a separate TU or link dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SID_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SID_UTILS_H

#include "hdr/types/gid_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Maximum SID size in bytes (15 sub-authorities × 4 + 8 header = 68).
inline constexpr ULONG MAX_SID_SIZE = 68;

/// Extract the POSIX numeric ID from the last sub-authority of a SID.
/// Returns 0 for null or empty SIDs.
LIBC_INLINE uid_t sid_to_uid(const SID *sid) {
  if (!sid || sid->SubAuthorityCount == 0)
    return 0;
  return static_cast<uid_t>(sid->SubAuthority[sid->SubAuthorityCount - 1]);
}

/// Build a domain-relative SID for a given numeric ID by copying the current
/// process token's user SID domain prefix and replacing the final RID.
///
/// This queries the process token's TokenUser to obtain the domain prefix,
/// so it works regardless of which domain the process belongs to. The caller
/// must provide a buffer of at least MAX_SID_SIZE bytes.
///
/// Returns true on success, false if the token query fails or the SID
/// structure is too small (< 2 sub-authorities).
LIBC_INLINE bool id_to_sid(unsigned long id, SID *out_sid) {
  alignas(8) UCHAR token_buf[256];
  ULONG needed = 0;
  NTSTATUS status = ::NtQueryInformationToken(
      NtCurrentProcessToken(), TokenUser, token_buf, sizeof(token_buf),
      &needed);
  if (!NT_SUCCESS(status))
    return false;

  const SID *user_sid =
      reinterpret_cast<const TOKEN_USER *>(token_buf)->User.Sid;
  if (!user_sid || user_sid->SubAuthorityCount < 2)
    return false;

  out_sid->Revision = 1;
  out_sid->SubAuthorityCount = user_sid->SubAuthorityCount;
  out_sid->IdentifierAuthority = user_sid->IdentifierAuthority;
  for (UCHAR i = 0; i < user_sid->SubAuthorityCount - 1; ++i)
    out_sid->SubAuthority[i] = user_sid->SubAuthority[i];
  out_sid->SubAuthority[user_sid->SubAuthorityCount - 1] =
      static_cast<ULONG>(id);
  return true;
}

/// Convenience: build a SID from a uid_t.
LIBC_INLINE bool uid_to_sid(uid_t uid, SID *out_sid) {
  return id_to_sid(static_cast<unsigned long>(uid), out_sid);
}

/// Convenience: build a SID from a gid_t.
LIBC_INLINE bool gid_to_sid(gid_t gid, SID *out_sid) {
  return id_to_sid(static_cast<unsigned long>(gid), out_sid);
}

/// Convert a uid to a username string via SID → RtlConvertSidToUnicodeString.
/// Strips the domain prefix (DOMAIN\user → user) for use with S4U logon.
/// Returns the username length in WCHARs, or 0 on failure.
LIBC_INLINE size_t uid_to_username(uid_t uid, WCHAR *name_buf,
                                   size_t name_cap) {
  alignas(8) UCHAR sid_buf[MAX_SID_SIZE];
  auto *sid = reinterpret_cast<SID *>(sid_buf);
  if (!uid_to_sid(uid, sid))
    return 0;

  UNICODE_STRING us = {};
  NTSTATUS status = ::RtlConvertSidToUnicodeString(&us, sid, 1);
  if (!NT_SUCCESS(status))
    return 0;

  // Find the last backslash to strip the domain prefix.
  const WCHAR *start = us.Buffer;
  size_t total_len = us.Length / sizeof(WCHAR);
  for (size_t i = total_len; i > 0; --i) {
    if (us.Buffer[i - 1] == u'\\') {
      start = &us.Buffer[i];
      total_len -= i;
      break;
    }
  }

  if (total_len == 0 || total_len >= name_cap) {
    ::RtlFreeUnicodeString(&us);
    return 0;
  }

  for (size_t i = 0; i < total_len; ++i)
    name_buf[i] = start[i];
  name_buf[total_len] = u'\0';
  ::RtlFreeUnicodeString(&us);
  return total_len;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_SID_UTILS_H
