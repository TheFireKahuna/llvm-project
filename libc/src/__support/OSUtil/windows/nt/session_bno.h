//===-- Session-scoped \BaseNamedObjects path builder ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// All NT named objects owned by libc (SysV IPC, FIFOs, POSIX semaphores,
// ALPC ports) live in \Sessions\<id>\BaseNamedObjects\<leaf>, not in the
// bare \BaseNamedObjects\ directory. Reasons:
//
//   1. Non-admin user processes cannot CREATE objects in \BaseNamedObjects\
//      (NtCreateSectionEx returns STATUS_ACCESS_DENIED). That directory is
//      the session-0 global namespace and requires SeCreateGlobalPrivilege.
//      Win32's CreateFileMapping/CreateMutex/etc. automatically prepend
//      \Sessions\<N>\ when no "Global\" prefix is given; direct Nt* calls
//      do not, so we must prefix explicitly.
//
//   2. POSIX isolation: SysV IPC, named sems, FIFOs are process-group /
//      per-user objects, not machine-global. Session scoping matches the
//      expected blast radius.
//
// The session id is read at PCB Phase 0 from PEB->SessionId and sealed
// read-only with Zone 0, so a corrupted PEB cannot redirect names across
// session boundaries.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SESSION_BNO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SESSION_BNO_H

#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/nt_wchar_converter.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Longest decimal representation of a 32-bit session id.
inline constexpr size_t SESSION_ID_MAX_DIGITS = 10;

// "\Sessions\" (10) + up to 10 session-id digits + "\BaseNamedObjects\" (18).
inline constexpr size_t SESSION_BNO_PREFIX_MAX_LEN =
    10 + SESSION_ID_MAX_DIGITS + 18;

// Write "\Sessions\<N>\BaseNamedObjects\" into the stream. Does NOT write a
// trailing NUL — the caller appends the leaf and terminates. On stream
// overflow the stream's err flag is set; caller checks overflow() after.
LIBC_INLINE void write_session_bno_prefix(WStringStream &ss) {
  ss << u"\\Sessions\\" << g_pcb.zone0.session_id()
     << u"\\BaseNamedObjects\\";
}

// Build "\Sessions\<N>\BaseNamedObjects\<leaf>" into |buf| from a UTF-8
// leaf. Returns the resulting WCHAR length excluding NUL, or 0 on failure
// (invalid leaf, overflow, or UTF-8 decode error).
//
// The leaf must be a plain object-manager leaf name: non-empty, contains
// neither '/' nor '\\'. A leading '/' or any embedded separator returns 0.
LIBC_INLINE size_t to_session_bno_path(cpp::string_view leaf, WCHAR *buf,
                                       size_t max_wchars) {
  if (leaf.empty() || leaf[0] == '/')
    return 0;
  for (size_t i = 0; i < leaf.size(); ++i) {
    if (leaf[i] == '/' || leaf[i] == '\\')
      return 0;
  }
  if (max_wchars == 0)
    return 0;

  WStringStream ss(cpp::span<WCHAR>(buf, max_wchars - 1));
  write_session_bno_prefix(ss);
  if (ss.overflow())
    return 0;

  // Convert the UTF-8 leaf directly after the prefix.
  size_t prefix_len = ss.str().size();
  int leaf_result = LIBC_NAMESPACE::windows::utf8_to_utf16(
      leaf.data(), leaf.size(), buf + prefix_len,
      max_wchars - prefix_len);
  if (leaf_result <= 0)
    return 0;
  size_t total = prefix_len + static_cast<size_t>(leaf_result);
  if (total >= max_wchars)
    return 0;
  buf[total] = u'\0';
  return total;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SESSION_BNO_H
