//===-- group database operations for Windows (NT-POSIX) ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements getgrgid/getgrnam by querying the process token's group list
// and a well-known group name table for Windows builtin groups.
//
// Group resolution strategy:
//   1. Check well-known group table (BUILTIN\*, NT AUTHORITY\*)
//   2. Query TokenGroups from the process token and match by GID/name
//   3. For names of non-well-known groups, use the SID string representation
//
// gr_mem is always an empty NULL-terminated array — we cannot enumerate
// group members without SAM database access, and POSIX does not require
// gr_mem to be complete.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/group_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/types/gid_t.h"
#include "include/llvm-libc-types/struct_group.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string_types.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/process/buf_writer.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/io/string_utils.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

//===----------------------------------------------------------------------===//
// Well-known group table
//===----------------------------------------------------------------------===//
//
// Windows builtin groups have fixed SIDs and names. We map the GID (last
// sub-authority of the SID) to the POSIX group name. This table covers the
// groups most commonly encountered in POSIX software.
//
// SID structure reference:
//   BUILTIN groups:      S-1-5-32-<RID>  (sub-authorities: [32, RID])
//   NT AUTHORITY groups: S-1-5-<RID>     (sub-authority: [RID])
//   Everyone:            S-1-1-0         (sub-authority: [0])
//
// With our sid_to_uid convention (last sub-authority), these map to:
//   Administrators = 544, Users = 545, SYSTEM = 18, etc.

struct WellKnownGroup {
  gid_t gid;
  const char *name;
};

// Sorted by gid for binary search potential, but the table is small enough
// that linear scan is fine.
constexpr WellKnownGroup WELL_KNOWN_GROUPS[] = {
    // NT AUTHORITY well-known SIDs (S-1-5-<N>)
    {11, "Authenticated Users"},  // S-1-5-11
    {18, "SYSTEM"},               // S-1-5-18 (root equivalent)
    {19, "LOCAL SERVICE"},        // S-1-5-19
    {20, "NETWORK SERVICE"},      // S-1-5-20
    // BUILTIN groups (S-1-5-32-<N>)
    {544, "Administrators"},      // S-1-5-32-544
    {545, "Users"},               // S-1-5-32-545
    {546, "Guests"},              // S-1-5-32-546
    {547, "Power Users"},         // S-1-5-32-547
    {551, "Backup Operators"},    // S-1-5-32-551
    {555, "Remote Desktop Users"},// S-1-5-32-555
    {568, "IIS_IUSRS"},           // S-1-5-32-568
};
constexpr size_t NUM_WELL_KNOWN =
    sizeof(WELL_KNOWN_GROUPS) / sizeof(WELL_KNOWN_GROUPS[0]);

// Look up a gid in the well-known table. Returns the name, or nullptr.
const char *wellknown_name_for_gid(gid_t gid) {
  for (size_t i = 0; i < NUM_WELL_KNOWN; ++i) {
    if (WELL_KNOWN_GROUPS[i].gid == gid)
      return WELL_KNOWN_GROUPS[i].name;
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Token group query
//===----------------------------------------------------------------------===//

// Query the process token for all groups. Returns a heap-allocated
// TOKEN_GROUPS on success (caller must free with page_free), or nullptr.
// If the groups fit on the stack buffer, copies them there instead and
// returns the stack pointer (caller checks before freeing).
TOKEN_GROUPS *query_token_groups(void *scratch_buf, size_t scratch_size,
                                 bool *used_heap) {
  HANDLE token =
      g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);
  HANDLE target = token ? token : NtCurrentProcessToken();

  ULONG needed = 0;
  NTSTATUS status =
      ::NtQueryInformationToken(target, TokenGroups, nullptr, 0, &needed);
  // Accept both STATUS_BUFFER_TOO_SMALL and STATUS_INFO_LENGTH_MISMATCH,
  // as either can be returned depending on the Windows version.
  if (status != STATUS_BUFFER_TOO_SMALL &&
      status != STATUS_INFO_LENGTH_MISMATCH)
    return nullptr;
  if (needed == 0)
    return nullptr;

  void *buf;
  if (needed <= scratch_size) {
    buf = scratch_buf;
    *used_heap = false;
  } else {
    buf = page_alloc(static_cast<size_t>(needed));
    if (!buf)
      return nullptr;
    *used_heap = true;
  }

  status =
      ::NtQueryInformationToken(target, TokenGroups, buf, needed, &needed);
  if (!NT_SUCCESS(status)) {
    if (*used_heap)
      page_free(buf);
    return nullptr;
  }

  return reinterpret_cast<TOKEN_GROUPS *>(buf);
}

// Check if a group SID should be visible as a POSIX group.
// Filters out logon SIDs, integrity SIDs, and deny-only SIDs.
bool is_posix_visible_group(ULONG attrs) {
  if (!(attrs & SE_GROUP_ENABLED))
    return false;
  if (attrs & (SE_GROUP_LOGON_ID | SE_GROUP_INTEGRITY |
               SE_GROUP_USE_FOR_DENY_ONLY))
    return false;
  return true;
}

// BufWriter is shared with passwd_ops.cpp — see buf_writer.h.

// Convert a SID to its string representation for use as a group name fallback.
// Writes into dst, returns length in WCHARs, or 0 on failure.
size_t sid_to_name_string(SID *sid, WCHAR *dst, size_t dst_cap) {
  UNICODE_STRING us = {};
  NTSTATUS status = ::RtlConvertSidToUnicodeString(&us, sid, 1);
  if (!NT_SUCCESS(status))
    return 0;

  size_t len = us.Length / sizeof(WCHAR);
  if (len >= dst_cap) {
    ::RtlFreeUnicodeString(&us);
    return 0;
  }

  __builtin_memcpy(dst, us.Buffer, len * sizeof(WCHAR));
  dst[len] = u'\0';
  ::RtlFreeUnicodeString(&us);
  return len;
}

// Core: fill a group struct from a name string, gid, and buffer.
int fill_group_result(gid_t gid, const char *name, struct group *grp,
                      char *buf, size_t buflen) {
  BufWriter w(buf, buflen);

  grp->gr_name = w.put_str(name);
  if (!grp->gr_name)
    return ERANGE;

  grp->gr_passwd = w.put_str("*");
  if (!grp->gr_passwd)
    return ERANGE;

  // gr_mem: empty NULL-terminated array.
  grp->gr_mem = w.put_ptr_array(1);
  if (!grp->gr_mem)
    return ERANGE;
  grp->gr_mem[0] = nullptr;

  grp->gr_gid = gid;
  return 0;
}

// Fill group from a SID name string (wide) and gid.
int fill_group_result_wide(gid_t gid, const WCHAR *name_wide,
                           size_t name_len, struct group *grp, char *buf,
                           size_t buflen) {
  BufWriter w(buf, buflen);

  grp->gr_name = w.put_wide(name_wide, name_len);
  if (!grp->gr_name)
    return ERANGE;

  grp->gr_passwd = w.put_str("*");
  if (!grp->gr_passwd)
    return ERANGE;

  grp->gr_mem = w.put_ptr_array(1);
  if (!grp->gr_mem)
    return ERANGE;
  grp->gr_mem[0] = nullptr;

  grp->gr_gid = gid;
  return 0;
}

} // namespace

//===----------------------------------------------------------------------===//
// fill_group_gid — look up group by gid
//===----------------------------------------------------------------------===//

int fill_group_gid(gid_t gid, struct group *grp, char *buf, size_t buflen) {
  // 1. Check well-known table first — these always exist regardless of token.
  const char *wk_name = wellknown_name_for_gid(gid);
  if (wk_name)
    return fill_group_result(gid, wk_name, grp, buf, buflen);

  // 2. Query token groups and find the matching SID by RID.
  auto tg_s = byte_scratch(1024);
  if (!tg_s)
    return ENOENT;
  bool used_heap = false;
  TOKEN_GROUPS *groups =
      query_token_groups(tg_s.data(), tg_s.size(), &used_heap);
  if (!groups)
    return ENOENT;

  int result = ENOENT;
  for (ULONG i = 0; i < groups->GroupCount; ++i) {
    if (!is_posix_visible_group(groups->Groups[i].Attributes))
      continue;

    auto *sid = reinterpret_cast<SID *>(groups->Groups[i].Sid);
    gid_t candidate = static_cast<gid_t>(sid_to_uid(sid));
    if (candidate != gid)
      continue;

    // Found it. Convert SID to string for the group name.
    WCHAR sid_str[256];
    size_t sid_len = sid_to_name_string(sid, sid_str, 256);
    if (sid_len > 0)
      result = fill_group_result_wide(gid, sid_str, sid_len, grp, buf, buflen);
    else
      result = ENOENT;
    break;
  }

  if (used_heap)
    page_free(groups);
  return result;
}

//===----------------------------------------------------------------------===//
// fill_group_name — look up group by name
//===----------------------------------------------------------------------===//

int fill_group_name(const char *name, struct group *grp, char *buf,
                    size_t buflen) {
  if (!name)
    return EINVAL;

  // 1. Check well-known table (case-insensitive match).
  //    Use the canonical name from the table, not the caller's input.
  for (size_t i = 0; i < NUM_WELL_KNOWN; ++i) {
    if (internal::string_util::ascii_streq_icase(WELL_KNOWN_GROUPS[i].name,
                                                 name))
      return fill_group_result(WELL_KNOWN_GROUPS[i].gid,
                               WELL_KNOWN_GROUPS[i].name, grp, buf, buflen);
  }

  // 2. Query token groups and try to match the SID string against the name.
  //    This handles cases where the caller passes a SID string directly
  //    (e.g., "S-1-5-21-xxx-1001") as the group name.
  auto tg_s = byte_scratch(1024);
  if (!tg_s)
    return ENOENT;
  bool used_heap = false;
  TOKEN_GROUPS *groups =
      query_token_groups(tg_s.data(), tg_s.size(), &used_heap);
  if (!groups)
    return ENOENT;

  int result = ENOENT;
  for (ULONG i = 0; i < groups->GroupCount; ++i) {
    if (!is_posix_visible_group(groups->Groups[i].Attributes))
      continue;

    auto *sid = reinterpret_cast<SID *>(groups->Groups[i].Sid);

    WCHAR sid_str[256];
    size_t sid_len = sid_to_name_string(sid, sid_str, 256);
    if (sid_len == 0)
      continue;

    // Case-insensitive compare: wide SID string vs UTF-8 name.
    bool match = true;
    size_t j = 0;
    for (; j < sid_len && name[j] != '\0'; ++j) {
      WCHAR wc = sid_str[j];
      auto nc = static_cast<unsigned char>(name[j]);
      if (wc >= u'A' && wc <= u'Z')
        wc += 32;
      unsigned char lc = nc;
      if (lc >= 'A' && lc <= 'Z')
        lc += 32;
      if (static_cast<unsigned>(wc) != static_cast<unsigned>(lc)) {
        match = false;
        break;
      }
    }
    if (match && j == sid_len && name[j] == '\0') {
      gid_t gid = static_cast<gid_t>(sid_to_uid(sid));
      result = fill_group_result_wide(gid, sid_str, sid_len, grp, buf, buflen);
      break;
    }
  }

  if (used_heap)
    page_free(groups);
  return result;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
