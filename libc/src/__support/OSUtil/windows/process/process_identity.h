//===-- Process-wide POSIX identity state for Windows ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX three-ID model (real, effective, saved) implemented on top of Windows
// token impersonation. Identity state lives in the Process Control Block
// (g_pcb) as cpp::Atomic<> fields — lock-free reads for getuid/geteuid/etc.,
// sequenced stores for setuid/seteuid/etc.
//
// Read access: include process_control_block.h and read g_pcb.identity.*.
// Write access: only identity_init(), identity_fork_reinit(), the set*id
//               engines in identity_ops.cpp, and set_exec_ids() below.
//
// This header provides:
//   - Privilege level constants (PRIV_NONE / PRIV_ADMIN / PRIV_TCB)
//   - Function declarations for identity lifecycle and token operations
//
// The ProcessIdentity struct is dissolved — fields live directly in the PCB.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_IDENTITY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_IDENTITY_H

#include "hdr/types/gid_t.h"
#include "hdr/types/uid_t.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_identity {

// Privilege level of the current process, determined at init.
// Stored in g_pcb.identity.privilege_level as cpp::Atomic<uint8_t>.
inline constexpr uint8_t PRIV_NONE = 0;  // Unprivileged user.
inline constexpr uint8_t PRIV_ADMIN = 1; // Elevated admin (no SeTcb).
inline constexpr uint8_t PRIV_TCB = 2;   // Has SeTcbPrivilege (SYSTEM).

// Initialize identity from the current process token. Idempotent.
// Writes g_pcb.{real_uid, eff_uid, saved_uid, real_gid, eff_gid, saved_gid,
// impersonation_token, privilege_level, identity_initialized}.
// Called by identity_startup_init() (Phase 2 of __libc_dll_init()).
void identity_init();

// Fork reinit: close cached token (invalid across fork), re-query identity.
// Called by identity_fork_reinit() from libc_fork_reinit().
void identity_fork_reinit();

// Set/clear impersonation token on a thread.
NTSTATUS apply_impersonation(HANDLE thread_handle, HANDLE token);
NTSTATUS clear_impersonation(HANDLE thread_handle);

// Acquire an impersonation token for uid via S4U logon.
// Requires SeTcbPrivilege. Returns STATUS_SUCCESS or an NT error.
NTSTATUS acquire_token_for_uid(uid_t uid, HANDLE *out_token);

// Create a restricted token with admin groups disabled and dangerous
// privileges removed. Used for permanent privilege drop in setuid().
NTSTATUS drop_privileges(HANDLE source_token, HANDLE *restricted_token);

// Duplicate the current impersonation token as a primary token suitable
// for NtSetInformationProcess(ProcessAccessToken). Returns nullptr if no
// impersonation is active (caller should use the process token as-is).
// Caller must NtClose the returned handle.
HANDLE get_child_primary_token();

// Update effective and saved IDs for setuid/setgid-bit exec. Per POSIX,
// exec of a setuid binary sets effective_uid = saved_uid = file owner uid,
// keeping real_uid unchanged. Returns 0 on success, errno on failure.
int set_exec_ids(uid_t file_uid, gid_t file_gid, bool setuid_bit,
                 bool setgid_bit);

} // namespace windows_identity
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_IDENTITY_H
