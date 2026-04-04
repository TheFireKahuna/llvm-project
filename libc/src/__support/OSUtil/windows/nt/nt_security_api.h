//===-- NT security, token, and registry API declarations ---- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECURITY_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECURITY_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_security_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Registry — NtOpenKeyEx / NtQueryValueKey
//===----------------------------------------------------------------------===//

// Opens an existing registry key. Security information in ObjectAttributes
// is ignored — the key's security descriptor is not checked against it.
// ObjectAttributes.ObjectName is the key path (e.g. \Registry\Machine\...).
// OpenOptions: 0, or REG_OPTION_OPEN_LINK (0x08) to open a symbolic link
// key itself rather than following it to its target.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenKeyEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess,
            POBJECT_ATTRIBUTES ObjectAttributes, ULONG OpenOptions);

// Retrieves the value of a registry key entry.
// ValueName identifies which value to query (case-insensitive).
// KeyValueInformationClass selects the output structure layout.
// On STATUS_BUFFER_TOO_SMALL, ResultLength receives the required size.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryValueKey(HANDLE KeyHandle, PCUNICODE_STRING ValueName,
                ULONG KeyValueInformationClass,
                PVOID KeyValueInformation, ULONG Length,
                PULONG ResultLength);

// Enumerates subkeys of a registry key by zero-based index.
// KeyHandle must have KEY_ENUMERATE_SUB_KEYS access.
// KeyInformationClass selects the output structure layout.
// Returns STATUS_NO_MORE_ENTRIES when Index >= number of subkeys.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtEnumerateKey(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass,
               PVOID KeyInformation, ULONG Length, PULONG ResultLength);

//===----------------------------------------------------------------------===//
// Security — SID, ACL, Security Descriptors
//===----------------------------------------------------------------------===//

// Retrieves a copy of an object's security descriptor in self-relative form.
// Handle must have READ_CONTROL access (for DACL/owner/group) or
// ACCESS_SYSTEM_SECURITY (for SACL). On STATUS_BUFFER_TOO_SMALL,
// LengthNeeded receives the required buffer size.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQuerySecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
                      SECURITY_DESCRIPTOR *SecurityDescriptor,
                      ULONG Length, ULONG *LengthNeeded);

// Sets an object's security state. Handle must have WRITE_DAC (for DACL),
// WRITE_OWNER (for owner/group), or ACCESS_SYSTEM_SECURITY (for SACL).
// The SecurityDescriptor may be absolute or self-relative form.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetSecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
                    SECURITY_DESCRIPTOR *SecurityDescriptor);

// Returns the length in bytes of a valid SID, including all sub-authorities.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR ULONG RtlLengthSid(SID *Sid);
// Compares two SIDs for exact equality (revision, authority, sub-authorities).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOLEAN RtlEqualSid(SID *Sid1, SID *Sid2);
// Copies SourceSid into DestinationSid. DestinationSidLength must be >=
// RtlLengthSid(SourceSid). Returns STATUS_BUFFER_TOO_SMALL on overflow.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlCopySid(ULONG DestinationSidLength, SID *DestinationSid, SID *SourceSid);

// Converts a SID to its string form (e.g. "S-1-5-21-...").
// If AllocateDestinationString is TRUE, ntdll allocates the buffer and the
// caller must free it with RtlFreeUnicodeString. If FALSE, UnicodeString
// must already point to a sufficiently large buffer.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlConvertSidToUnicodeString(UNICODE_STRING *UnicodeString, SID *Sid,
                             BOOLEAN AllocateDestinationString);

// Frees a UNICODE_STRING buffer previously allocated by ntdll routines
// (RtlConvertSidToUnicodeString, RtlAnsiStringToUnicodeString, etc.).
// Sets Buffer to NULL and Length/MaximumLength to 0 after freeing.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlFreeUnicodeString(UNICODE_STRING *UnicodeString);

//===----------------------------------------------------------------------===//
// Token Manipulation
//===----------------------------------------------------------------------===//

// Opens the access token associated with a process.
// ProcessHandle must have PROCESS_QUERY_INFORMATION access.
// DesiredAccess specifies the requested token access rights (TOKEN_QUERY, etc.).
// HandleAttributes: 0 from user mode (OBJ_KERNEL_HANDLE is kernel-only).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenProcessTokenEx(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                     ULONG HandleAttributes, HANDLE *TokenHandle);

// Retrieves information about a token.
// TokenInformationClass selects the structure layout (TokenUser = 1, etc.).
// If TokenInformationClass is TokenSource (7), the handle must have
// TOKEN_QUERY_SOURCE access; all others require TOKEN_QUERY.
// On STATUS_BUFFER_TOO_SMALL, ReturnLength receives the required size.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryInformationToken(HANDLE TokenHandle, ULONG TokenInformationClass,
                        PVOID TokenInformation, ULONG TokenInformationLength,
                        ULONG *ReturnLength);

// Creates a new token that duplicates an existing one.
// EffectiveOnly: if TRUE, only currently enabled groups/privileges are copied.
// Type: TokenTypePrimary (1) or TokenTypeImpersonation (2).
// The new token's impersonation level is taken from ObjectAttributes
// (SecurityQualityOfService), defaulting to SecurityImpersonation.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtDuplicateToken(HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess,
                 OBJECT_ATTRIBUTES *ObjectAttributes, BOOLEAN EffectiveOnly,
                 ULONG Type, HANDLE *NewTokenHandle);

// Creates a restricted version of an existing token (Win8+, extended variant).
// ExistingTokenHandle must have TOKEN_DUPLICATE access.
// SidsToDisable: these SIDs become deny-only (cannot grant access).
// PrivilegesToDelete: removed from the new token.
// RestrictedSids: added as restricting SIDs — access is granted only if
//   both the normal SIDs and the restricting SIDs allow it.
// The claims/device parameters allow filtering claim attributes and device
// groups (pass 0/NULL for all six if not needed).
// The new token inherits the type (primary/impersonation) of the existing one.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtFilterTokenEx(HANDLE ExistingTokenHandle, ULONG Flags,
                TOKEN_GROUPS *SidsToDisable,
                TOKEN_PRIVILEGES *PrivilegesToDelete,
                TOKEN_GROUPS *RestrictedSids,
                ULONG DisableUserClaimsCount,
                PCUNICODE_STRING UserClaimsToDisable,
                ULONG DisableDeviceClaimsCount,
                PCUNICODE_STRING DeviceClaimsToDisable,
                TOKEN_GROUPS *DeviceGroupsToDisable,
                PVOID RestrictedUserAttributes,
                PVOID RestrictedDeviceAttributes,
                TOKEN_GROUPS *RestrictedDeviceGroups,
                HANDLE *NewTokenHandle);

// Modifies information stored in a token.
// Requires TOKEN_ADJUST_DEFAULT for most classes, TOKEN_ADJUST_SESSIONID
// for TokenSessionId (plus SeTcbPrivilege).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetInformationToken(HANDLE TokenHandle, ULONG TokenInformationClass,
                      PVOID TokenInformation, ULONG TokenInformationLength);

// Opens the impersonation token of a thread.
// ThreadHandle must have THREAD_QUERY_INFORMATION access.
// OpenAsSelf: if TRUE, the access check uses the process token instead of
// the calling thread's impersonation token.
// HandleAttributes: 0 from user mode (OBJ_KERNEL_HANDLE is kernel-only).
// Returns STATUS_NO_TOKEN if the thread is not impersonating.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenThreadTokenEx(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                    BOOLEAN OpenAsSelf, ULONG HandleAttributes,
                    HANDLE *TokenHandle);

// Enables or disables privileges in a token.
// TokenHandle must have TOKEN_ADJUST_PRIVILEGES access.
// If DisableAllPrivileges is TRUE, all privileges are disabled and NewState
// is ignored. Otherwise, NewState specifies which privileges to modify.
// PreviousState receives the old privilege state (for later restoration);
// pass NULL + BufferLength 0 if not needed.
// Returns STATUS_NOT_ALL_ASSIGNED if some requested privileges were not held.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAdjustPrivilegesToken(HANDLE TokenHandle, BOOLEAN DisableAllPrivileges,
                        TOKEN_PRIVILEGES *NewState, ULONG BufferLength,
                        TOKEN_PRIVILEGES *PreviousState,
                        ULONG *ReturnLength);

// Enables or disables groups in a token.
// TokenHandle must have TOKEN_ADJUST_GROUPS access.
// If ResetToDefault is TRUE, all groups are reset to their default state
// and NewState is ignored. Otherwise, NewState specifies which groups to
// enable (SE_GROUP_ENABLED) or disable. Mandatory groups (SE_GROUP_MANDATORY)
// cannot be disabled. Returns STATUS_CANT_DISABLE_MANDATORY on attempt.
// PreviousState receives the old group state; pass NULL + BufferLength 0
// if not needed.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAdjustGroupsToken(HANDLE TokenHandle, BOOLEAN ResetToDefault,
                    TOKEN_GROUPS *NewState, ULONG BufferLength,
                    TOKEN_GROUPS *PreviousState, ULONG *ReturnLength);

// Compares two tokens for equivalence. Both handles must have TOKEN_QUERY.
// Tokens are equal if they have the same user SID, logon session, and
// all groups/privileges match. Useful for geteuid()==getuid() semantics.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCompareTokens(HANDLE FirstTokenHandle, HANDLE SecondTokenHandle,
                BOOLEAN *Equal);

// Creates a lowbox (AppContainer) token from an existing token (Win8+).
// ExistingTokenHandle must have TOKEN_DUPLICATE access.
// PackageSid identifies the AppContainer — determines the isolated
// namespace (registry, filesystem, object manager).
// Capabilities grant access to specific resources (e.g. network, webcam).
// Handles are inherited into the lowbox token's handle table.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateLowBoxToken(HANDLE *TokenHandle, HANDLE ExistingTokenHandle,
                    ACCESS_MASK DesiredAccess,
                    OBJECT_ATTRIBUTES *ObjectAttributes, SID *PackageSid,
                    ULONG CapabilityCount,
                    SID_AND_ATTRIBUTES *Capabilities, ULONG HandleCount,
                    HANDLE *Handles);

// Causes a thread to impersonate the anonymous logon token.
// ThreadHandle must have THREAD_IMPERSONATE access.
// The anonymous token has no user SID and minimal access — useful for
// extreme privilege reduction. Revert via NtSetInformationThread with
// ThreadImpersonationToken = NULL.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtImpersonateAnonymousToken(HANDLE ThreadHandle);

// Retrieves security attribute information from a token.
// TokenHandle must have TOKEN_QUERY access.
// Attributes is an array of attribute names to query (NULL = query all).
// Buffer receives a TOKEN_SECURITY_ATTRIBUTES_INFORMATION structure.
// On STATUS_BUFFER_TOO_SMALL, ReturnLength receives the required size.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQuerySecurityAttributesToken(HANDLE TokenHandle,
                               PCUNICODE_STRING Attributes,
                               ULONG NumberOfAttributes, PVOID Buffer,
                               ULONG Length, ULONG *ReturnLength);

//===----------------------------------------------------------------------===//
// Access Checking
//===----------------------------------------------------------------------===//

// Determines whether a security descriptor grants the requested access
// to the client represented by ClientToken. This is the core of
// access()/faccessat() implementation on Windows.
// ClientToken must be an impersonation token with TOKEN_QUERY access
// (duplicate a primary token with SecurityIdentification if needed).
// GenericMapping translates GENERIC_* bits in DesiredAccess and ACE masks.
// PrivilegeSet receives any privileges used to grant access (e.g.
// SeSecurityPrivilege for SACL access); allocate at least
// sizeof(PRIVILEGE_SET) bytes.
// GrantedAccess receives the access rights actually granted.
// AccessStatus receives STATUS_SUCCESS if access is granted, or an
// error (typically STATUS_ACCESS_DENIED) if not.
// The function itself returns STATUS_SUCCESS unless parameters are invalid.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAccessCheck(SECURITY_DESCRIPTOR *SecurityDescriptor, HANDLE ClientToken,
              ACCESS_MASK DesiredAccess, GENERIC_MAPPING *GenericMapping,
              PRIVILEGE_SET *PrivilegeSet, ULONG *PrivilegeSetLength,
              ACCESS_MASK *GrantedAccess, NTSTATUS *AccessStatus);

// Checks whether a set of privileges are enabled in a token.
// ClientToken must have TOKEN_QUERY access.
// RequiredPrivileges specifies the privileges to check — set Control to
// PRIVILEGE_SET_ALL_NECESSARY to require all, or 0 to require any one.
// On return, each Privilege[i].Attributes has SE_PRIVILEGE_USED_FOR_ACCESS
// (0x80000000) set if that privilege was found enabled.
// Result receives TRUE if the privilege requirement is met.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtPrivilegeCheck(HANDLE ClientToken, PRIVILEGE_SET *RequiredPrivileges,
                 BOOLEAN *Result);

// Privilege acquisition helpers (ntdll.dll). Temporarily enables the
// requested privileges by impersonating a token with them enabled.
// Returns an opaque state handle that must be released via RtlReleasePrivilege.
// Flags:
//   RTL_ACQUIRE_PRIVILEGE_REVERT (0x01)  — revert a previous impersonation first
//   RTL_ACQUIRE_PRIVILEGE_PROCESS (0x02) — adjust the process token, not a
//                                          thread impersonation token
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlAcquirePrivilege(ULONG *Privilege, ULONG NumPrivileges, ULONG Flags,
                    PVOID *ReturnedState);
// Releases the privilege state acquired by RtlAcquirePrivilege — reverts
// the thread impersonation (or process token adjustment).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlReleasePrivilege(PVOID ReturnedState);

//===----------------------------------------------------------------------===//
// Extended Attributes (EAs) — for $LXMOD/$LXUID/$LXGID WSL metadata
//===----------------------------------------------------------------------===//

// Queries extended attributes on a file.
// FileHandle must have FILE_READ_EA access.
// Buffer receives FILE_FULL_EA_INFORMATION entries.
// EaList (FILE_GET_EA_INFORMATION array) filters which EAs to return;
// pass NULL + EaListLength 0 to retrieve all EAs.
// ReturnSingleEntry: if TRUE, returns at most one EA entry.
// EaIndex: 1-based index to start from (NULL = use RestartScan).
// RestartScan: if TRUE, starts enumeration from the beginning.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryEaFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
              PVOID Buffer, ULONG Length, BOOLEAN ReturnSingleEntry,
              PVOID EaList, ULONG EaListLength, PULONG EaIndex,
              BOOLEAN RestartScan);

// Sets extended attributes on a file.
// FileHandle must have FILE_WRITE_EA access.
// Buffer contains one or more FILE_FULL_EA_INFORMATION entries.
// To delete an EA, set EaValueLength to 0 for that name.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetEaFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
            PVOID Buffer, ULONG Length);

//===----------------------------------------------------------------------===//
// Token Creation — NtCreateTokenEx
//===----------------------------------------------------------------------===//

// NtCreateTokenEx — create a primary or impersonation token from scratch.
// Requires SE_CREATE_TOKEN_PRIVILEGE. This is the full setuid()/setgid()
// primitive: constructs a token with arbitrary user, groups, privileges,
// and (Win8+) security claim attributes.
//
// Type: TokenTypePrimary (1) for process tokens, TokenTypeImpersonation (2)
//       for thread impersonation tokens.
// AuthenticationId: logon session LUID (e.g. SYSTEM_LUID or from LogonUser).
// ExpirationTime: token expiry (MAXLONGLONG for no expiry).
// User: TOKEN_USER — the token's primary SID.
// Groups: TOKEN_GROUPS — group memberships and attributes (SE_GROUP_ENABLED, etc.).
// Privileges: TOKEN_PRIVILEGES — granted privileges.
// UserAttributes: optional TOKEN_SECURITY_ATTRIBUTES_INFORMATION — user claim
//                 attributes for conditional ACEs (Win8+). nullptr if not needed.
// DeviceAttributes: optional device claim attributes. nullptr if not needed.
// DeviceGroups: optional device group SIDs. nullptr if not needed.
// MandatoryPolicy: optional TOKEN_MANDATORY_POLICY. nullptr for default.
// Owner: optional TOKEN_OWNER — default owner for new objects. nullptr = user SID.
// PrimaryGroup: TOKEN_PRIMARY_GROUP — primary group for new objects.
// DefaultDacl: optional TOKEN_DEFAULT_DACL. nullptr = no default DACL.
// Source: TOKEN_SOURCE — 8-char identifier of the token creator.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateTokenEx(HANDLE *TokenHandle, ACCESS_MASK DesiredAccess,
                PCOBJECT_ATTRIBUTES ObjectAttributes, ULONG Type,
                LUID *AuthenticationId, LARGE_INTEGER *ExpirationTime,
                TOKEN_USER *User, TOKEN_GROUPS *Groups,
                TOKEN_PRIVILEGES *Privileges,
                TOKEN_SECURITY_ATTRIBUTES_INFORMATION *UserAttributes,
                TOKEN_SECURITY_ATTRIBUTES_INFORMATION *DeviceAttributes,
                TOKEN_GROUPS *DeviceGroups,
                TOKEN_MANDATORY_POLICY *MandatoryPolicy,
                TOKEN_OWNER *Owner, TOKEN_PRIMARY_GROUP *PrimaryGroup,
                TOKEN_DEFAULT_DACL *DefaultDacl, TOKEN_SOURCE *Source);

//===----------------------------------------------------------------------===//
// LUID Generation — NtAllocateLocallyUniqueId
//===----------------------------------------------------------------------===//

// NtAllocateLocallyUniqueId — generate a locally unique identifier.
// LUIDs are 64-bit monotonically increasing values, unique within a single
// boot session. Used for privilege LUIDs, authentication IDs, and any
// context requiring a machine-local unique key.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtAllocateLocallyUniqueId(LUID *Luid);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECURITY_API_H
