//===-- LSA client API declarations for Windows -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declarations for sspicli.dll (LSA Security Support Provider Client Interface)
// used by setuid/seteuid for S4U (Service-for-User) logon. S4U allows a
// process with SeTcbPrivilege to obtain a token for any local user without
// knowing their password — the NT equivalent of POSIX setuid.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SSPICLI_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SSPICLI_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/ntdll.h"

//===----------------------------------------------------------------------===//
// LSA Types
//===----------------------------------------------------------------------===//

// ANSI string for LSA package names.
struct LSA_STRING {
  USHORT Length;
  USHORT MaximumLength;
  CHAR *Buffer;
};

// Opaque handle returned by LsaConnectUntrusted / LsaRegisterLogonProcess.
using LSA_HANDLE = HANDLE;

// Logon types (SECURITY_LOGON_TYPE). S4U uses Network (3).
inline constexpr ULONG LOGON_TYPE_NETWORK = 3;

// MSV1_0 authentication package message types.
inline constexpr ULONG MsV1_0S4ULogon = 12;

// MSV1_0_S4U_LOGON — the auth info buffer for S4U logon.
// Tells MSV1_0 to produce a token for the named user without a password.
struct MSV1_0_S4U_LOGON {
  ULONG MessageType;           // Must be MsV1_0S4ULogon (12).
  ULONG Flags;                 // Reserved, must be 0.
  UNICODE_STRING UserPrincipalName; // Target account name.
  UNICODE_STRING DomainName;   // Domain or machine name (empty = local).
};

// QUOTA_LIMITS — returned by LsaLogonUser, we discard it.
struct QUOTA_LIMITS {
  SIZE_T PagedPoolLimit;
  SIZE_T NonPagedPoolLimit;
  SIZE_T MinimumWorkingSetSize;
  SIZE_T MaximumWorkingSetSize;
  SIZE_T PagefileLimit;
  LARGE_INTEGER TimeLimit;
};

//===----------------------------------------------------------------------===//
// LSA Function Declarations (sspicli.dll)
//===----------------------------------------------------------------------===//

extern "C" {

// Connect to LSA without registering as a logon process. Returns an
// LSA_HANDLE usable with LsaLookupAuthenticationPackage and LsaLogonUser.
// No special privilege required for the connection itself.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LsaConnectUntrusted(LSA_HANDLE *LsaHandle);

// Resolve an authentication package by name (e.g.
// "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0") to a numeric ID.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LsaLookupAuthenticationPackage(LSA_HANDLE LsaHandle,
                               LSA_STRING *PackageName,
                               ULONG *AuthenticationPackage);

// Perform a logon. For S4U: pass an MSV1_0_S4U_LOGON as AuthenticationInfo
// with type Network. Requires SeTcbPrivilege for impersonation-level tokens.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LsaLogonUser(LSA_HANDLE LsaHandle, LSA_STRING *OriginName, ULONG LogonType,
             ULONG AuthenticationPackage, PVOID AuthenticationInformation,
             ULONG AuthenticationInformationLength,
             TOKEN_GROUPS *LocalGroups, TOKEN_SOURCE *SourceContext,
             PVOID *ProfileBuffer, ULONG *ProfileBufferLength, LUID *LogonId,
             HANDLE *Token, QUOTA_LIMITS *Quotas, NTSTATUS *SubStatus);

// Free the profile buffer returned by LsaLogonUser.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LsaFreeReturnBuffer(PVOID Buffer);

// Close an LSA connection handle.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
LsaDeregisterLogonProcess(LSA_HANDLE LsaHandle);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SSPICLI_H
