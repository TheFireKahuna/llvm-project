//===-- Process identity implementation for Windows ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the POSIX three-ID model on Windows using NT token manipulation
// and S4U (Service-for-User) logon via LSA. S4U allows a process running as
// SYSTEM (SeTcbPrivilege) to obtain a token for any local user without
// knowing their password. This is the same mechanism Cygwin 3.0+ uses.
//
// Unprivileged processes can only change effective uid/gid back to their
// real or saved values — any other change returns EPERM, matching POSIX.
//
// Identity state is stored directly in the Process Control Block (g_pcb)
// as cpp::Atomic<> fields. The former ProcessIdentity singleton struct
// has been dissolved — there is no g_identity or get_process_identity().
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/process_identity.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/security/secure_zero.h"
#include "src/__support/OSUtil/windows/security/sspicli.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_identity {

// SID helpers are now in sid_utils.h — use internal::sid_to_uid,
// internal::uid_to_sid, internal::uid_to_username, etc.

//===----------------------------------------------------------------------===//
// Privilege detection
//===----------------------------------------------------------------------===//

// Scan a TOKEN_PRIVILEGES buffer for a specific enabled privilege.
static bool scan_privileges(const TOKEN_PRIVILEGES *tp,
                            ULONG privilege_luid_low) {
  for (ULONG i = 0; i < tp->PrivilegeCount; ++i) {
    if (tp->Privileges[i].Luid.LowPart == privilege_luid_low &&
        tp->Privileges[i].Luid.HighPart == 0 &&
        (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED))
      return true;
  }
  return false;
}

// Check whether a specific privilege is present and enabled in the token.
// The initial stack buffer handles up to ~28 privileges. SYSTEM tokens can
// have 29+, so we retry with a heap allocation if the first call fails with
// STATUS_BUFFER_TOO_SMALL.
static bool has_privilege(HANDLE token, ULONG privilege_luid_low) {
  alignas(8) UCHAR buf[512];
  ULONG needed = 0;
  NTSTATUS status = ::NtQueryInformationToken(
      token, TokenPrivileges, buf, sizeof(buf), &needed);
  if (NT_SUCCESS(status))
    return scan_privileges(reinterpret_cast<TOKEN_PRIVILEGES *>(buf),
                           privilege_luid_low);

  if (status != STATUS_BUFFER_TOO_SMALL)
    return false;

  // Retry with exact size via page_alloc (the foundation allocator
  // available before malloc/pools are initialized).
  void *heap_buf = internal::page_alloc(static_cast<size_t>(needed));
  if (!heap_buf)
    return false;

  status = ::NtQueryInformationToken(
      token, TokenPrivileges, heap_buf, needed, &needed);
  bool result = NT_SUCCESS(status) &&
                scan_privileges(reinterpret_cast<TOKEN_PRIVILEGES *>(heap_buf),
                                privilege_luid_low);

  internal::page_free(heap_buf);
  return result;
}

static uint8_t detect_privilege_level() {
  HANDLE token = NtCurrentProcessToken();

  // SeTcbPrivilege (SYSTEM or service granted TCB) — can do S4U logon.
  if (has_privilege(token, SE_TCB_PRIVILEGE))
    return PRIV_TCB;

  // Check for elevated admin token.
  ULONG elev_type = 0;
  ULONG needed = 0;
  NTSTATUS status = ::NtQueryInformationToken(
      token, TokenElevationType, &elev_type, sizeof(elev_type), &needed);
  if (NT_SUCCESS(status) && elev_type == TokenElevationTypeFull)
    return PRIV_ADMIN;

  return PRIV_NONE;
}

//===----------------------------------------------------------------------===//
// Initialization
//===----------------------------------------------------------------------===//

void identity_init() {
  // Idempotent — relaxed load is fine, worst case we init twice and the
  // second writer sees the same token values. All stores below are to
  // g_pcb fields (the PCB is the single source of truth).
  if (g_pcb.identity.initialized.load(cpp::MemoryOrder::RELAXED))
    return;

  HANDLE token = NtCurrentProcessToken();

  // Query owner SID -> real_uid.
  // 256 bytes covers all practical TOKEN_USER structures (max SID ≈ 68 bytes).
  auto owner_buf = windows::byte_scratch(256);
  uid_t uid = 0;
  if (owner_buf) {
    ULONG needed = 0;
    NTSTATUS status = ::NtQueryInformationToken(
        token, TokenUser, owner_buf.data(),
        static_cast<ULONG>(owner_buf.size_bytes()), &needed);
    if (NT_SUCCESS(status)) {
      auto *tu = reinterpret_cast<TOKEN_USER *>(owner_buf.data());
      uid = internal::sid_to_uid(tu->User.Sid);
    }
  }

  // Query primary group SID -> real_gid.
  auto group_buf = windows::byte_scratch(256);
  gid_t gid = 0;
  if (group_buf) {
    ULONG needed = 0;
    NTSTATUS status = ::NtQueryInformationToken(
        token, TokenPrimaryGroup, group_buf.data(),
        static_cast<ULONG>(group_buf.size_bytes()),
        &needed);
    if (NT_SUCCESS(status)) {
      auto *tpg = reinterpret_cast<TOKEN_PRIMARY_GROUP *>(group_buf.data());
      gid = static_cast<gid_t>(internal::sid_to_uid(tpg->PrimaryGroup));
    }
  }

  g_pcb.identity.real_uid.store(uid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.eff_uid.store(uid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.saved_uid.store(uid, cpp::MemoryOrder::RELAXED);

  g_pcb.identity.real_gid.store(gid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.eff_gid.store(gid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.saved_gid.store(gid, cpp::MemoryOrder::RELAXED);

  g_pcb.identity.impersonation_token.store(nullptr,
                                           cpp::MemoryOrder::RELAXED);
  g_pcb.identity.privilege_level.store(detect_privilege_level(),
                                       cpp::MemoryOrder::RELAXED);

  g_pcb.identity.initialized.store(1, cpp::MemoryOrder::RELEASE);
}

//===----------------------------------------------------------------------===//
// Fork reinit
//===----------------------------------------------------------------------===//

void identity_fork_reinit() {
  // The parent's impersonation token was created with internal_oa_with_sqos()
  // (non-inheritable), so it doesn't exist in the child's handle table.
  // Just null the stale pointer — no NtClose.
  g_pcb.identity.impersonation_token.store(nullptr,
                                           cpp::MemoryOrder::RELAXED);

  // Re-query identity from the child's process token. The child may have
  // a different token if the parent used CreateProcessAsUser.
  g_pcb.identity.initialized.store(0, cpp::MemoryOrder::RELAXED);
  identity_init();
}

//===----------------------------------------------------------------------===//
// Thread impersonation
//===----------------------------------------------------------------------===//

NTSTATUS apply_impersonation(HANDLE thread_handle, HANDLE token) {
  return ::NtSetInformationThread(thread_handle, ThreadImpersonationToken,
                                  &token, sizeof(token));
}

NTSTATUS clear_impersonation(HANDLE thread_handle) {
  HANDLE null_token = nullptr;
  return ::NtSetInformationThread(thread_handle, ThreadImpersonationToken,
                                  &null_token, sizeof(null_token));
}

//===----------------------------------------------------------------------===//
// S4U logon — acquire an impersonation token for a given uid
//===----------------------------------------------------------------------===//

NTSTATUS acquire_token_for_uid(uid_t uid, HANDLE *out_token) {
  *out_token = nullptr;

  // Step 1: Connect to LSA.
  LSA_HANDLE lsa = nullptr;
  NTSTATUS status = ::LsaConnectUntrusted(&lsa);
  if (!NT_SUCCESS(status))
    return status;

  // Step 2: Look up the MSV1_0 authentication package.
  char pkg_name[] = "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0";
  LSA_STRING pkg_str;
  pkg_str.Buffer = pkg_name;
  pkg_str.Length = sizeof(pkg_name) - 1;
  pkg_str.MaximumLength = sizeof(pkg_name);

  ULONG auth_package = 0;
  status = ::LsaLookupAuthenticationPackage(lsa, &pkg_str, &auth_package);
  if (!NT_SUCCESS(status)) {
    ::LsaDeregisterLogonProcess(lsa);
    return status;
  }

  // Step 3: Build the S4U logon info.
  WCHAR user_buf[256];
  size_t user_len = internal::uid_to_username(uid, user_buf, 256);
  if (user_len == 0) {
    ::LsaDeregisterLogonProcess(lsa);
    return STATUS_NO_SUCH_USER;
  }

  MSV1_0_S4U_LOGON s4u;
  s4u.MessageType = MsV1_0S4ULogon;
  s4u.Flags = 0;
  windows::nt_wstring_view upn_wsv(user_buf, user_len);
  s4u.UserPrincipalName = *upn_wsv.unicode_string();
  windows::nt_wstring_view empty_domain;
  s4u.DomainName = *empty_domain.unicode_string();

  // Step 4: Perform the logon.
  char origin[] = "llvm-libc";
  LSA_STRING origin_str;
  origin_str.Buffer = origin;
  origin_str.Length = sizeof(origin) - 1;
  origin_str.MaximumLength = sizeof(origin);

  TOKEN_SOURCE source;
  // 8-byte source name, NUL-padded.
  source.SourceName[0] = 'l';
  source.SourceName[1] = 'l';
  source.SourceName[2] = 'v';
  source.SourceName[3] = 'm';
  source.SourceName[4] = 'l';
  source.SourceName[5] = 'i';
  source.SourceName[6] = 'b';
  source.SourceName[7] = 'c';
  source.SourceIdentifier.LowPart = 0;
  source.SourceIdentifier.HighPart = 0;

  PVOID profile = nullptr;
  ULONG profile_len = 0;
  LUID logon_id = {};
  windows::ScopedNtHandle new_token;
  QUOTA_LIMITS quotas = {};
  NTSTATUS sub_status = 0;

  status = ::LsaLogonUser(lsa, &origin_str, LOGON_TYPE_NETWORK, auth_package,
                           &s4u, sizeof(s4u), nullptr, &source, &profile,
                           &profile_len, &logon_id, new_token.put(), &quotas,
                           &sub_status);

  if (profile) {
    // Wipe the MSV1_0 profile blob before returning it to LSA.
    // LsaFreeReturnBuffer only releases the allocation; it does not
    // zero the pages, so residual authentication context would
    // otherwise stay resident in L1/L2 until the line is evicted
    // naturally. secure_zero + CLFLUSHOPT closes that window.
    internal::secure_zero(profile, profile_len);
    ::LsaFreeReturnBuffer(profile);
  }
  ::LsaDeregisterLogonProcess(lsa);

  if (!NT_SUCCESS(status))
    return status;

  // The token from LsaLogonUser is a primary token. Duplicate as
  // impersonation at SecurityImpersonation level for thread use.
  // SECURITY_QUALITY_OF_SERVICE is typed as PVOID in OBJECT_ATTRIBUTES;
  // we build the struct inline with the documented layout.
  struct {
    ULONG Length;
    ULONG ImpersonationLevel;
    UCHAR ContextTrackingMode;
    BOOLEAN EffectiveOnly;
  } qos = {sizeof(qos), SecurityImpersonation, 1 /* DYNAMIC_TRACKING */, 0};
  auto oa = windows::internal_oa_with_sqos(&qos);

  HANDLE imp_token = nullptr;
  status = ::NtDuplicateToken(new_token.get(),
                              TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                              &oa, 0, TokenTypeImpersonation, &imp_token);
  new_token.reset();

  if (!NT_SUCCESS(status))
    return status;

  *out_token = imp_token;
  return STATUS_SUCCESS;
}

//===----------------------------------------------------------------------===//
// Privilege dropping
//===----------------------------------------------------------------------===//

NTSTATUS drop_privileges(HANDLE source_token, HANDLE *restricted_token) {
  // NtFilterTokenEx with DISABLE_MAX_PRIVILEGE removes all privileges
  // except SE_CHANGE_NOTIFY_PRIVILEGE and marks admin SIDs as deny-only.
  return ::NtFilterTokenEx(source_token, DISABLE_MAX_PRIVILEGE,
                           nullptr, nullptr, nullptr,
                           0, nullptr, 0, nullptr, nullptr,
                           nullptr, nullptr, nullptr, restricted_token);
}

//===----------------------------------------------------------------------===//
// Child process token
//===----------------------------------------------------------------------===//

HANDLE get_child_primary_token() {
  HANDLE imp_token = g_pcb.identity.impersonation_token.load(
      cpp::MemoryOrder::ACQUIRE);
  if (!imp_token)
    return nullptr; // No impersonation active — child inherits process token.

  // Duplicate as primary token for NtSetInformationProcess(ProcessAccessToken).
  auto oa = windows::internal_oa();
  HANDLE primary = nullptr;
  NTSTATUS status = ::NtDuplicateToken(
      imp_token, MAXIMUM_ALLOWED, &oa, 0, TokenTypePrimary, &primary);
  if (!NT_SUCCESS(status))
    return nullptr;
  return primary;
}

//===----------------------------------------------------------------------===//
// Setuid-bit exec
//===----------------------------------------------------------------------===//

int set_exec_ids(uid_t file_uid, gid_t file_gid, bool setuid_bit,
                 bool setgid_bit) {
  if (setuid_bit && file_uid != g_pcb.identity.eff_uid.load(
                                    cpp::MemoryOrder::RELAXED)) {
    // Acquire token for the file owner.
    HANDLE new_token = nullptr;
    NTSTATUS status = acquire_token_for_uid(file_uid, &new_token);
    if (!NT_SUCCESS(status))
      return status == STATUS_PRIVILEGE_NOT_HELD ? EPERM : EPERM;

    windows::ScopedNtHandle old_token(
        g_pcb.identity.impersonation_token.exchange(
            new_token, cpp::MemoryOrder::ACQ_REL));

    // POSIX: exec of setuid binary sets effective and saved, not real.
    g_pcb.identity.eff_uid.store(file_uid, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.saved_uid.store(file_uid, cpp::MemoryOrder::RELAXED);
  }

  if (setgid_bit && file_gid != g_pcb.identity.eff_gid.load(
                                    cpp::MemoryOrder::RELAXED)) {
    // Modify the primary group on the active token.
    alignas(8) UCHAR sid_buf[internal::MAX_SID_SIZE];
    auto *gid_sid = reinterpret_cast<SID *>(sid_buf);
    if (internal::gid_to_sid(file_gid, gid_sid)) {
      TOKEN_PRIMARY_GROUP tpg;
      tpg.PrimaryGroup = gid_sid;
      HANDLE token =
          g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);
      HANDLE target = token ? token : NtCurrentProcessToken();
      ::NtSetInformationToken(target, TokenPrimaryGroup, &tpg, sizeof(tpg));
    }
    g_pcb.identity.eff_gid.store(file_gid, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.saved_gid.store(file_gid, cpp::MemoryOrder::RELAXED);
  }

  return 0;
}

} // namespace windows_identity
} // namespace LIBC_NAMESPACE_DECL

// Phase 2: Process identity (uid/gid/privilege, parent_pid).
int LIBC_NAMESPACE::internal::identity_startup_init() {
  using namespace LIBC_NAMESPACE;
  windows_identity::identity_init();

  // Last Zone 0b write: parent PID via ProcessBasicInformation.
  // Must complete before pcb_seal_readonly_b().
  PROCESS_BASIC_INFORMATION pbi;
  NTSTATUS st = ::NtQueryInformationProcess(
      NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
  internal::PcbInitAccess::set_parent_pid(
      NT_SUCCESS(st)
          ? static_cast<DWORD>(pbi.InheritedFromUniqueProcessId)
          : 0);

  return 0;
}

void LIBC_NAMESPACE::internal::identity_fork_reinit() {
  LIBC_NAMESPACE::windows_identity::identity_fork_reinit();
}

LIBC_REGISTER_FORK_REINIT(identity,
                          ::LIBC_NAMESPACE::internal::kForkPrioIdentity,
                          &::LIBC_NAMESPACE::internal::identity_fork_reinit)
