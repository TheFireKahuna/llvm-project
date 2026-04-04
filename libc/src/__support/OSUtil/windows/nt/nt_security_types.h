//===-- NT security, token, and registry types --------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECURITY_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECURITY_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Registry — NtOpenKeyEx / NtQueryValueKey
//===----------------------------------------------------------------------===//

// Registry key access rights (combinable).
// Read the data, type, and name of value entries under this key.
inline constexpr ACCESS_MASK KEY_QUERY_VALUE = 0x0001;

// KEY_VALUE_INFORMATION_CLASS for NtQueryValueKey.
// Reference: ntregapi.h KEY_VALUE_INFORMATION_CLASS enum.
//
// KeyValueBasicInformation (0) — KEY_VALUE_BASIC_INFORMATION (type + name)
// KeyValueFullInformation  (1) — KEY_VALUE_FULL_INFORMATION (type + name + data)
// KeyValuePartialInformation (2) — type + data, no name (most common)
// KeyValueFullInformationAlign64 (3) — 8-byte aligned variant
// KeyValuePartialInformationAlign64 (4) — 8-byte aligned, no TitleIndex
inline constexpr ULONG KeyValuePartialInformation = 2;

// Returned by NtQueryValueKey(KeyValuePartialInformation).
// Variable-length: Data[] extends beyond the declared size.
struct KEY_VALUE_PARTIAL_INFORMATION {
  ULONG TitleIndex;  // Reserved — always 0 in practice.
  ULONG Type;        // Registry value type: REG_SZ (1), REG_DWORD (4), etc.
  ULONG DataLength;  // Size in bytes of the Data[] array.
  UCHAR Data[1];     // Variable-length value data.
};

// Registry key access rights — enumerate sub-keys.
inline constexpr ACCESS_MASK KEY_ENUMERATE_SUB_KEYS = 0x0008;

// KEY_INFORMATION_CLASS for NtEnumerateKey.
// KeyBasicInformation (0) — last-write time + name
inline constexpr ULONG KeyBasicInformation = 0;

// Returned by NtEnumerateKey(KeyBasicInformation).
// Variable-length: Name[] extends beyond the declared size.
struct KEY_BASIC_INFORMATION {
  LARGE_INTEGER LastWriteTime;
  ULONG TitleIndex;   // Reserved — always 0 in practice.
  ULONG NameLength;   // Size in bytes of the Name[] array (not including NUL).
  WCHAR Name[1];      // Variable-length key name (not NUL-terminated).
};

// Registry value types (REG_*). Only those we use are declared.
// REG_SZ: null-terminated UTF-16 string.
inline constexpr ULONG REG_SZ = 1;
// REG_EXPAND_SZ: null-terminated UTF-16 string with environment variable refs.
inline constexpr ULONG REG_EXPAND_SZ = 2;
// REG_DWORD: 32-bit unsigned integer (little-endian).
inline constexpr ULONG REG_DWORD = 4;

//===----------------------------------------------------------------------===//
// Security — SID, ACL, Security Descriptors
//===----------------------------------------------------------------------===//

// A SID_IDENTIFIER_AUTHORITY identifies the issuing authority for a SID.
// Well-known values:
//   {0,0,0,0,0,1} — SECURITY_WORLD_SID_AUTHORITY (Everyone)
//   {0,0,0,0,0,5} — SECURITY_NT_AUTHORITY (NT accounts)
//   {0,0,0,0,0,16} — SECURITY_MANDATORY_LABEL_AUTHORITY
struct SID_IDENTIFIER_AUTHORITY {
  UCHAR Value[6];
};

// Security Identifier — uniquely identifies a user, group, or trust level.
// Binary layout: Revision (1) + SubAuthorityCount + IdentifierAuthority +
// SubAuthority[SubAuthorityCount]. Total size varies; use RtlLengthSid().
// Maximum size: SECURITY_MAX_SID_SIZE (68 bytes, 15 sub-authorities).
struct SID {
  UCHAR Revision;                            // Must be 1 (SID_REVISION).
  UCHAR SubAuthorityCount;                   // 0..15 (SECURITY_MAX_SUB_AUTHORITY_COUNT).
  SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
  ULONG SubAuthority[1];                     // Variable length.
};

// Group SID attribute flags — used in SID_AND_ATTRIBUTES.Attributes for
// TOKEN_GROUPS, TOKEN_MANDATORY_LABEL, and group membership queries.
inline constexpr ULONG SE_GROUP_MANDATORY = 0x00000001;
inline constexpr ULONG SE_GROUP_ENABLED_BY_DEFAULT = 0x00000002;
inline constexpr ULONG SE_GROUP_ENABLED = 0x00000004;
inline constexpr ULONG SE_GROUP_OWNER = 0x00000008;
inline constexpr ULONG SE_GROUP_USE_FOR_DENY_ONLY = 0x00000010;
inline constexpr ULONG SE_GROUP_INTEGRITY = 0x00000020;
inline constexpr ULONG SE_GROUP_INTEGRITY_ENABLED = 0x00000040;
inline constexpr ULONG SE_GROUP_RESOURCE = 0x20000000;
inline constexpr ULONG SE_GROUP_LOGON_ID = 0xC0000000;

// SID paired with attribute flags. Used in TOKEN_USER, TOKEN_GROUPS, and
// TOKEN_MANDATORY_LABEL structures.
struct SID_AND_ATTRIBUTES {
  SID *Sid;
  ULONG Attributes;
};

// Access Control List — header for an ordered list of ACEs.
// An ACL is variable-length: the ACE entries follow this header.
// AclSize includes the header and all ACEs. Maximum AclSize is 65535.
struct ACL {
  UCHAR AclRevision;  // ACL_REVISION (2) or ACL_REVISION_DS (4).
  UCHAR Sbz1;         // Padding — must be 0.
  USHORT AclSize;     // Total size in bytes (header + all ACEs).
  USHORT AceCount;    // Number of ACEs in this ACL.
  USHORT Sbz2;        // Padding — must be 0.
};

// ACE header — common prefix for all ACE types.
struct ACE_HEADER {
  UCHAR AceType;   // ACCESS_ALLOWED_ACE_TYPE (0), ACCESS_DENIED_ACE_TYPE (1), etc.
  UCHAR AceFlags;  // Inheritance and audit flags (OBJECT_INHERIT_ACE, etc.).
  USHORT AceSize;  // Total size of this ACE in bytes (header + body).
};

// ACCESS_ALLOWED_ACE — grants the specified Mask to the identified SID.
// Variable-length: the SID bytes begin at SidStart and extend beyond it.
// AceSize = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + RtlLengthSid(sid).
struct ACCESS_ALLOWED_ACE {
  ACE_HEADER Header;
  ACCESS_MASK Mask;
  ULONG SidStart;  // First DWORD of the SID; remaining bytes follow.
};

// ACE type constants.
inline constexpr UCHAR ACCESS_ALLOWED_ACE_TYPE = 0;
inline constexpr UCHAR ACCESS_DENIED_ACE_TYPE = 1;

// ACL revision. ACL_REVISION (2) supports standard ACE types.
// ACL_REVISION_DS (4) is needed for object-specific ACEs (AD DS).
inline constexpr UCHAR ACL_REVISION = 2;

// Absolute-form security descriptor — contains pointers (not self-relative
// offsets). Use SECURITY_DESCRIPTOR_REVISION (1) for Revision.
// Self-relative form (SE_SELF_RELATIVE set) stores offsets instead of
// pointers and is used for serialization / IPC. NtQuerySecurityObject
// always returns self-relative form; convert for modification.
using SECURITY_DESCRIPTOR_CONTROL = USHORT;
// DACL is present in the descriptor. If clear, the system grants full access.
inline constexpr SECURITY_DESCRIPTOR_CONTROL SE_DACL_PRESENT = 0x0004;
// DACL is protected — inheritable ACEs from the parent are not applied.
inline constexpr SECURITY_DESCRIPTOR_CONTROL SE_DACL_PROTECTED = 0x1000;
// Descriptor is in self-relative format (offsets, not pointers).
inline constexpr SECURITY_DESCRIPTOR_CONTROL SE_SELF_RELATIVE = 0x8000;

struct SECURITY_DESCRIPTOR {
  UCHAR Revision;                        // Must be SECURITY_DESCRIPTOR_REVISION (1).
  UCHAR Sbz1;                            // Padding — must be 0.
  SECURITY_DESCRIPTOR_CONTROL Control;   // SE_DACL_PRESENT, SE_SELF_RELATIVE, etc.
  SID *Owner;                            // Owner SID (NULL = not present).
  SID *Group;                            // Primary group SID (NULL = not present).
  ACL *Sacl;                             // System ACL — audit/integrity (NULL = none).
  ACL *Dacl;                             // Discretionary ACL — access control (NULL = full access).
};

inline constexpr UCHAR SECURITY_DESCRIPTOR_REVISION = 1;

// SECURITY_INFORMATION — bitmask selecting which parts of a security
// descriptor to query or set via NtQuerySecurityObject / NtSetSecurityObject.
using SECURITY_INFORMATION = ULONG;
// Query/set the owner SID.
inline constexpr SECURITY_INFORMATION OWNER_SECURITY_INFORMATION = 0x00000001;
// Query/set the primary group SID.
inline constexpr SECURITY_INFORMATION GROUP_SECURITY_INFORMATION = 0x00000002;
// Query/set the DACL.
inline constexpr SECURITY_INFORMATION DACL_SECURITY_INFORMATION = 0x00000004;

//===----------------------------------------------------------------------===//
// Token Manipulation
//===----------------------------------------------------------------------===//

// TOKEN_INFORMATION_CLASS — selects the structure returned or set by
// NtQueryInformationToken / NtSetInformationToken.
// Reference: ntseapi.h TOKEN_INFORMATION_CLASS enum.
//
// Only the classes we use are declared. The full enum has 51 values.
//  1  TokenUser              — q: TOKEN_USER (user SID + attributes)
//  2  TokenGroups            — q: TOKEN_GROUPS (group SIDs + attributes)
//  3  TokenPrivileges        — q: TOKEN_PRIVILEGES (LUID + attributes array)
//  4  TokenOwner             — qs: TOKEN_OWNER (default owner SID)
//  5  TokenPrimaryGroup      — qs: TOKEN_PRIMARY_GROUP (default group SID)
//  6  TokenDefaultDacl       — qs: TOKEN_DEFAULT_DACL (DACL for new objects)
//  7  TokenSource            — q: TOKEN_SOURCE (8-char name + LUID)
//  8  TokenType              — q: ULONG (TokenTypePrimary or TokenTypeImpersonation)
//  9  TokenImpersonationLevel — q: ULONG (SecurityAnonymous..SecurityDelegation)
// 12  TokenSessionId         — qs: ULONG (requires SeTcbPrivilege to set)
// 18  TokenElevationType     — q: ULONG (default/full/limited)
// 19  TokenLinkedToken        — qs: TOKEN_LINKED_TOKEN (requires SeCreateTokenPrivilege)
inline constexpr ULONG TokenUser = 1;
inline constexpr ULONG TokenGroups = 2;
inline constexpr ULONG TokenPrivileges = 3;
inline constexpr ULONG TokenOwner = 4;
inline constexpr ULONG TokenPrimaryGroup = 5;
inline constexpr ULONG TokenDefaultDacl = 6;
inline constexpr ULONG TokenSource = 7;
inline constexpr ULONG TokenType = 8;
inline constexpr ULONG TokenImpersonationLevel = 9;
inline constexpr ULONG TokenSessionId = 12;
inline constexpr ULONG TokenElevationType = 18;
inline constexpr ULONG TokenLinkedToken = 19;

// TOKEN_ELEVATION_TYPE — returned by NtQueryInformationToken(TokenElevationType).
// Default: process is not using a split token (UAC disabled or non-admin).
// Full: process is running with the full (elevated) admin token.
// Limited: process is running with the filtered (non-elevated) user token.
inline constexpr DWORD TokenElevationTypeDefault = 1;
inline constexpr DWORD TokenElevationTypeFull = 2;
inline constexpr DWORD TokenElevationTypeLimited = 3;

// Token access rights — used with NtOpenProcessTokenEx / NtDuplicateToken.
// Required to assign the token as a process's primary token.
inline constexpr ACCESS_MASK TOKEN_ASSIGN_PRIMARY = 0x0001;
// Required to duplicate the token.
inline constexpr ACCESS_MASK TOKEN_DUPLICATE = 0x0002;
// Required to attach the token to an impersonating thread.
inline constexpr ACCESS_MASK TOKEN_IMPERSONATE = 0x0004;
// Required to query the token (NtQueryInformationToken).
inline constexpr ACCESS_MASK TOKEN_QUERY = 0x0008;
// Required to enable/disable privileges (NtAdjustPrivilegesToken).
inline constexpr ACCESS_MASK TOKEN_ADJUST_PRIVILEGES = 0x0020;
// Required to enable/disable groups (NtAdjustGroupsToken).
inline constexpr ACCESS_MASK TOKEN_ADJUST_GROUPS = 0x0040;
// Required to change default owner, group, or DACL (NtSetInformationToken).
inline constexpr ACCESS_MASK TOKEN_ADJUST_DEFAULT = 0x0080;
// MAXIMUM_ALLOWED is defined in nt_types.h.

// TOKEN_TYPE — primary tokens are attached to processes; impersonation
// tokens are attached to threads and borrow another identity temporarily.
inline constexpr ULONG TokenTypePrimary = 1;
inline constexpr ULONG TokenTypeImpersonation = 2;

// SECURITY_IMPERSONATION_LEVEL — controls what the server thread can do
// with the client's identity when impersonating.
// Anonymous: server cannot obtain client identification.
// Identification: server can query client identity but not impersonate.
// Impersonation: server can impersonate on the local system.
// Delegation: server can impersonate on remote systems.
inline constexpr ULONG SecurityAnonymous = 0;
inline constexpr ULONG SecurityIdentification = 1;
inline constexpr ULONG SecurityImpersonation = 2;
inline constexpr ULONG SecurityDelegation = 3;

// THREADINFOCLASS value for NtSetInformationThread — sets or clears the
// impersonation token on a thread. Pass NULL handle to revert to self.
inline constexpr ULONG ThreadImpersonationToken = 5;

// Privilege constants — LUID values for well-known privileges.
// Reference: ntseapi.h SE_*_PRIVILEGE defines.
//
// These are the numeric LUID values assigned at boot. The full list goes
// from SE_MIN_WELL_KNOWN_PRIVILEGE (2) to SE_MAX_WELL_KNOWN_PRIVILEGE (36).
// Only those we use are declared here.
//
// Required to create a primary token (NtCreateToken).
inline constexpr ULONG SE_CREATE_TOKEN_PRIVILEGE = 2;
// Required to assign the primary token of a process.
inline constexpr ULONG SE_ASSIGNPRIMARYTOKEN_PRIVILEGE = 3;
// Required to act as part of the Trusted Computer Base.
inline constexpr ULONG SE_TCB_PRIVILEGE = 7;
// Required to increase the base priority of a process.
inline constexpr ULONG SE_INC_BASE_PRIORITY_PRIVILEGE = 14;
// Required to impersonate a client after authentication.
inline constexpr ULONG SE_IMPERSONATE_PRIVILEGE = 29;
// Required to allocate more memory for applications (increase working set).
inline constexpr ULONG SE_INCREASE_WORKING_SET_PRIVILEGE = 33;

// PROCESSINFOCLASS value for NtSetInformationProcess — assigns a new
// primary token to a process. Takes a PROCESS_ACCESS_TOKEN structure.
// Requires SE_ASSIGNPRIMARYTOKEN_PRIVILEGE.
inline constexpr ULONG ProcessAccessToken = 9;

// TOKEN_USER — returned by NtQueryInformationToken(TokenUser).
// Contains the SID of the user account associated with the token.
struct TOKEN_USER {
  SID_AND_ATTRIBUTES User;
};

// TOKEN_PRIMARY_GROUP — returned/set by NtQueryInformationToken(TokenPrimaryGroup).
// Specifies the default primary group SID for new objects.
struct TOKEN_PRIMARY_GROUP {
  SID *PrimaryGroup;
};

// TOKEN_GROUPS — variable-length array of SID_AND_ATTRIBUTES.
// Returned by NtQueryInformationToken(TokenGroups) and used as input
// to NtFilterTokenEx (SidsToDisable, RestrictedSids, DeviceGroupsToDisable).
struct TOKEN_GROUPS {
  ULONG GroupCount;
  SID_AND_ATTRIBUTES Groups[1]; // Variable length.
};

// Locally Unique Identifier — 64-bit value unique on this boot session.
// Used for privilege LUIDs, logon session IDs, and token source IDs.
struct LUID {
  ULONG LowPart;
  LONG HighPart;
};

// LUID paired with attribute flags. Used in TOKEN_PRIVILEGES.
// Attributes:
//   SE_PRIVILEGE_ENABLED_BY_DEFAULT (0x01) — enabled when token was created
//   SE_PRIVILEGE_ENABLED (0x02)            — currently active
//   SE_PRIVILEGE_REMOVED (0x04)            — privilege has been permanently removed
//   SE_PRIVILEGE_USED_FOR_ACCESS (0x80000000) — used during last access check
struct LUID_AND_ATTRIBUTES {
  LUID Luid;
  ULONG Attributes;
};

// SE_PRIVILEGE_ENABLED — the privilege is active in the token.
inline constexpr ULONG SE_PRIVILEGE_ENABLED = 0x00000002;

// TOKEN_PRIVILEGES — variable-length array of privilege LUIDs and attributes.
// Returned by NtQueryInformationToken(TokenPrivileges) and used as input
// to NtAdjustPrivilegesToken and NtFilterTokenEx (PrivilegesToDelete).
struct TOKEN_PRIVILEGES {
  ULONG PrivilegeCount;
  LUID_AND_ATTRIBUTES Privileges[1]; // Variable length.
};

// TOKEN_SOURCE — identifies who created the token (e.g. "User32 ", "NtLmSsp").
// SourceName is always 8 bytes, space-padded (not null-terminated).
struct TOKEN_SOURCE {
  CHAR SourceName[8];
  LUID SourceIdentifier;
};

// TOKEN_OWNER — default owner SID for new objects created by the token holder.
// Returned/set by NtQueryInformationToken/NtSetInformationToken(TokenOwner).
struct TOKEN_OWNER {
  SID *Owner;
};

// TOKEN_DEFAULT_DACL — default discretionary ACL for new objects.
// Returned/set by NtQueryInformationToken(TokenDefaultDacl).
struct TOKEN_DEFAULT_DACL {
  ACL *DefaultDacl; // nullptr = no default DACL (full access to everyone).
};

// TOKEN_MANDATORY_POLICY — mandatory integrity policy for the token.
// Controls how the integrity level affects access checks.
struct TOKEN_MANDATORY_POLICY {
  ULONG Policy;
};

inline constexpr ULONG TOKEN_MANDATORY_POLICY_OFF = 0x0;
inline constexpr ULONG TOKEN_MANDATORY_POLICY_NO_WRITE_UP = 0x1;
inline constexpr ULONG TOKEN_MANDATORY_POLICY_NEW_PROCESS_MIN = 0x2;

// TOKEN_SECURITY_ATTRIBUTES_INFORMATION — security claim attributes (Win8+).
// Used by NtCreateTokenEx for UserAttributes/DeviceAttributes parameters.
// The Attributes union points to a variable-length array of attribute entries.
struct TOKEN_SECURITY_ATTRIBUTE_V1 {
  UNICODE_STRING Name;
  USHORT ValueType;   // TOKEN_SECURITY_ATTRIBUTE_TYPE_*
  USHORT Reserved;
  ULONG Flags;        // TOKEN_SECURITY_ATTRIBUTE_*
  ULONG ValueCount;
  union {
    LONG64 *Int64;
    ULONG64 *Uint64;
    UNICODE_STRING *String;
    // Additional value types omitted — add as needed.
  } Values;
};

inline constexpr USHORT TOKEN_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1 = 1;

struct TOKEN_SECURITY_ATTRIBUTES_INFORMATION {
  USHORT Version; // Must be TOKEN_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1.
  USHORT Reserved;
  ULONG AttributeCount;
  TOKEN_SECURITY_ATTRIBUTE_V1 *AttributeV1;
};

// PROCESS_ACCESS_TOKEN — input for NtSetInformationProcess(ProcessAccessToken).
// Assigns a new primary token to a process. The process must have no threads
// running (or be the current process with appropriate privilege).
struct PROCESS_ACCESS_TOKEN {
  HANDLE Token;
  HANDLE Thread; // Must be NULL for primary token assignment.
};

// Pseudo-handle for the current process token (Win8+). Avoids the
// NtOpenProcessTokenEx + NtClose round-trip. Recognized by all token APIs.
// Similarly, (HANDLE)(LONG_PTR)-5 is the current thread token and
// (HANDLE)(LONG_PTR)-6 is the current effective token (thread if
// impersonating, otherwise process).
inline HANDLE NtCurrentProcessToken() {
  return reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4));
}

// NtFilterTokenEx flags — control how the restricted token is constructed.
// Disables all privileges except SE_CHANGE_NOTIFY_PRIVILEGE.
inline constexpr ULONG DISABLE_MAX_PRIVILEGE = 0x1;
// Stores TOKEN_SANDBOX_INERT in the token — AppLocker/SRP rules are skipped.
inline constexpr ULONG SANDBOX_INERT = 0x2;
// Creates a Limited User Account (LUA) token.
inline constexpr ULONG LUA_TOKEN = 0x4;
// Creates a write-restricted token — restricting SIDs apply only to write ACEs.
inline constexpr ULONG WRITE_RESTRICTED = 0x8;

//===----------------------------------------------------------------------===//
// Access Checking
//===----------------------------------------------------------------------===//

// GENERIC_MAPPING — maps generic access rights (GENERIC_READ, GENERIC_WRITE,
// GENERIC_EXECUTE, GENERIC_ALL) to object-specific access rights.
// Each object type defines its own mapping. For files:
//   GenericRead    = FILE_GENERIC_READ
//   GenericWrite   = FILE_GENERIC_WRITE
//   GenericExecute = FILE_GENERIC_EXECUTE
//   GenericAll     = FILE_ALL_ACCESS
// NtAccessCheck uses this to translate generic bits in DesiredAccess and
// in ACE masks before performing the access check.
struct GENERIC_MAPPING {
  ACCESS_MASK GenericRead;
  ACCESS_MASK GenericWrite;
  ACCESS_MASK GenericExecute;
  ACCESS_MASK GenericAll;
};

// PRIVILEGE_SET — variable-length array of privileges, used by NtAccessCheck
// (output: privileges used to grant access) and NtPrivilegeCheck (input:
// privileges to test).
// Control flags:
//   0 — at least one privilege in the set must be held
//   PRIVILEGE_SET_ALL_NECESSARY (1) — all privileges must be held
inline constexpr ULONG PRIVILEGE_SET_ALL_NECESSARY = 1;

struct PRIVILEGE_SET {
  ULONG PrivilegeCount;            // Number of elements in Privilege[].
  ULONG Control;                   // 0 or PRIVILEGE_SET_ALL_NECESSARY.
  LUID_AND_ATTRIBUTES Privilege[1]; // Variable length.
};

// ProcessQuotaLimits (class 1) — query or set working set sizes and flags.
inline constexpr ULONG ProcessQuotaLimits = 1;

// Extended quota limits structure — used with NtSetInformationProcess
// (ProcessQuotaLimits) to enforce hard working set limits.
// Set MinimumWorkingSetSize/MaximumWorkingSetSize and the corresponding
// QUOTA_LIMITS_HARDWS_*_ENABLE flags to lock the working set range.
struct QUOTA_LIMITS_EX {
  SIZE_T PagedPoolLimit;            // +0x00  Max paged pool usage (0 = no limit).
  SIZE_T NonPagedPoolLimit;         // +0x08  Max non-paged pool usage (0 = no limit).
  SIZE_T MinimumWorkingSetSize;     // +0x10  Minimum pages guaranteed in RAM.
  SIZE_T MaximumWorkingSetSize;     // +0x18  Maximum pages allowed in RAM.
  SIZE_T PagefileLimit;             // +0x20  Max pagefile usage (0 = no limit).
  LARGE_INTEGER TimeLimit;          // +0x28  Max CPU time (0 = no limit).
  SIZE_T WorkingSetLimit;           // +0x30  Reserved.
  SIZE_T Reserved2;                 // +0x38
  SIZE_T Reserved3;                 // +0x40
  SIZE_T Reserved4;                 // +0x48
  ULONG Flags;                      // +0x50  QUOTA_LIMITS_HARDWS_* flags.
  ULONG CpuRateLimit;              // +0x54  CPU rate limit (0 = disabled).
};                                  // sizeof = 0x58

// QUOTA_LIMITS_EX Flags for hard working set enforcement.
// Requires SE_INCREASE_WORKING_SET_PRIVILEGE for the calling process.
inline constexpr ULONG QUOTA_LIMITS_HARDWS_MIN_ENABLE  = 0x00000001;
inline constexpr ULONG QUOTA_LIMITS_HARDWS_MIN_DISABLE = 0x00000002;
inline constexpr ULONG QUOTA_LIMITS_HARDWS_MAX_ENABLE  = 0x00000004;
inline constexpr ULONG QUOTA_LIMITS_HARDWS_MAX_DISABLE = 0x00000008;

//===----------------------------------------------------------------------===//
// Extended Attributes (EAs) — for $LXMOD/$LXUID/$LXGID WSL metadata
//===----------------------------------------------------------------------===//

// FILE_FULL_EA_INFORMATION — used by NtSetEaFile and returned by NtQueryEaFile.
// Variable-length: the EA name (EaNameLength bytes + NUL terminator) is
// followed immediately by the EA value (EaValueLength bytes).
// Multiple entries are chained via NextEntryOffset (0 = last entry).
// Each entry is ULONG-aligned.
struct FILE_FULL_EA_INFORMATION {
  ULONG NextEntryOffset;   // Byte offset to next entry, or 0 if last.
  UCHAR Flags;             // FILE_NEED_EA (0x80) if EA is required for file open.
  UCHAR EaNameLength;      // Length of EaName, excluding the NUL terminator.
  USHORT EaValueLength;    // Length of the value bytes after EaName + NUL.
  CHAR EaName[1];          // NUL-terminated name; value follows at EaName[EaNameLength+1].
};

// FILE_GET_EA_INFORMATION — input filter for NtQueryEaFile (EaList parameter).
// Specifies which EA names to retrieve. Multiple entries chained via
// NextEntryOffset (0 = last).
struct FILE_GET_EA_INFORMATION {
  ULONG NextEntryOffset;
  UCHAR EaNameLength;
  CHAR EaName[1]; // NUL-terminated.
};

// FileStatLxInformation (class 70) — returns NTFS + WSL metadata in one call.
// Available since Redstone 4 (1803). Requires FILE_READ_ATTRIBUTES | FILE_READ_EA.
// The LxFlags field indicates which WSL fields are valid.
struct FILE_STAT_LX_INFORMATION {
  LARGE_INTEGER FileId;
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER AllocationSize;
  LARGE_INTEGER EndOfFile;
  ULONG FileAttributes;
  ULONG ReparseTag;
  ULONG NumberOfLinks;
  ACCESS_MASK EffectiveAccess;   // Access granted by the caller's token.
  ULONG LxFlags;                // Bitmask: LX_FILE_METADATA_HAS_UID/GID/MODE.
  ULONG LxUid;                  // WSL UID (valid if LX_FILE_METADATA_HAS_UID).
  ULONG LxGid;                  // WSL GID (valid if LX_FILE_METADATA_HAS_GID).
  ULONG LxMode;                 // WSL mode_t (valid if LX_FILE_METADATA_HAS_MODE).
  ULONG LxDeviceIdMajor;        // Device major number (for device files).
  ULONG LxDeviceIdMinor;        // Device minor number (for device files).
};

// LxFlags — indicate which WSL metadata fields contain valid data.
// Set via NtSetEaFile with the $LXUID/$LXGID/$LXMOD extended attributes.
inline constexpr ULONG LX_FILE_METADATA_HAS_UID = 0x1;
inline constexpr ULONG LX_FILE_METADATA_HAS_GID = 0x2;
inline constexpr ULONG LX_FILE_METADATA_HAS_MODE = 0x4;

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECURITY_TYPES_H
