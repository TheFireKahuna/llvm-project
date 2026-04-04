//===-- POSIX permission helpers for Windows --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Three-tier POSIX file permission model on Windows:
//   Tier 1: FILE_ATTRIBUTE_READONLY — works on every filesystem.
//   Tier 2: NTFS DACLs — enforced owner/group/other permissions.
//   Tier 3: $LXMOD/$LXUID/$LXGID EAs — bit-perfect mode, WSL interop.
//
// Each tier degrades gracefully. No filesystem detection — the NTSTATUS
// return codes tell us what the filesystem supports.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include "hdr/sys_stat_macros.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/uid_t.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_sec {

//===----------------------------------------------------------------------===//
// Process-wide umask
//===----------------------------------------------------------------------===//

// Stored in g_pcb.identity.umask (PCB Zone 1, mutable). Default 022 (octal) is set
// by pcb_startup_init() (Phase 0 of __libc_dll_init()). Uses __atomic builtins on plain unsigned
// for lock-free relaxed access — umask changes are rare and non-critical.

LIBC_INLINE mode_t get_umask() {
  return static_cast<mode_t>(
      g_pcb.identity.umask.load(cpp::MemoryOrder::RELAXED));
}

LIBC_INLINE mode_t set_umask(mode_t mask) {
  return static_cast<mode_t>(
      g_pcb.identity.umask.exchange(mask & 0777, cpp::MemoryOrder::RELAXED));
}

//===----------------------------------------------------------------------===//
// Well-known SIDs
//===----------------------------------------------------------------------===//

// S-1-1-0 (Everyone / World).
inline constexpr SID_IDENTIFIER_AUTHORITY WORLD_SID_AUTHORITY = {{0, 0, 0, 0, 0, 1}};

// Build the Everyone SID in a caller-provided buffer.
LIBC_INLINE void init_everyone_sid(SID *sid) {
  sid->Revision = 1;
  sid->SubAuthorityCount = 1;
  sid->IdentifierAuthority = WORLD_SID_AUTHORITY;
  sid->SubAuthority[0] = 0; // S-1-1-0
}

//===----------------------------------------------------------------------===//
// SID ↔ uid/gid mapping (WSL-compatible convention)
//===----------------------------------------------------------------------===//

// Local accounts: uid = RID (SubAuthority[last]).
// Domain accounts: uid = hash(domain) << 16 | (RID & 0xFFFF).
LIBC_INLINE uid_t sid_to_uid(SID *sid) {
  if (!sid || sid->SubAuthorityCount == 0)
    return 0;
  ULONG rid = sid->SubAuthority[sid->SubAuthorityCount - 1];
  // Local machine SIDs: S-1-5-21-x-y-z-RID. SubAuthorityCount == 4+.
  // For simplicity, use the RID directly — matches WSL for local accounts
  // and is deterministic for domain accounts.
  return static_cast<uid_t>(rid);
}

// Reverse mapping: construct a SID from a uid using the current user's
// domain prefix (S-1-5-21-X-Y-Z) with the uid as the final RID.
// out_sid must point to a buffer of at least MAX_SID_SIZE bytes.
inline constexpr ULONG MAX_SID_SIZE = 68; // 8 + 4*15 sub-authorities max.

LIBC_INLINE bool uid_to_sid(uid_t uid, SID *out_sid) {
  // Get the current user's SID to extract the domain prefix.
  alignas(8) UCHAR token_buf[256];
  ULONG needed = 0;
  NTSTATUS status = ::NtQueryInformationToken(
      NtCurrentProcessToken(), TokenUser, token_buf, sizeof(token_buf),
      &needed);
  if (!NT_SUCCESS(status))
    return false;

  SID *user_sid = reinterpret_cast<TOKEN_USER *>(token_buf)->User.Sid;
  if (!user_sid || user_sid->SubAuthorityCount < 2)
    return false;

  // Copy the domain prefix (all sub-authorities except the last RID),
  // then append the target uid as the new RID.
  out_sid->Revision = 1;
  out_sid->SubAuthorityCount = user_sid->SubAuthorityCount;
  out_sid->IdentifierAuthority = user_sid->IdentifierAuthority;
  for (UCHAR i = 0; i < user_sid->SubAuthorityCount - 1; i++)
    out_sid->SubAuthority[i] = user_sid->SubAuthority[i];
  out_sid->SubAuthority[user_sid->SubAuthorityCount - 1] =
      static_cast<ULONG>(uid);
  return true;
}

//===----------------------------------------------------------------------===//
// Mode ↔ ACCESS_MASK translation
//===----------------------------------------------------------------------===//

// Callback type for rwx-to-ACCESS_MASK mapping. Each object type has its own
// specific access rights, so callers pick the appropriate mapper.
using AccessMaskFn = ACCESS_MASK (*)(int);

// Map a single rwx triplet (0-7) to NT file access rights.
LIBC_INLINE ACCESS_MASK mode_bits_to_access_mask(int rwx) {
  ACCESS_MASK mask = 0;
  if (rwx & 4) // r
    mask |= FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES;
  if (rwx & 2) // w
    mask |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
            FILE_WRITE_ATTRIBUTES | DELETE_ACCESS;
  if (rwx & 1) // x
    mask |= FILE_EXECUTE;
  // READ_CONTROL and SYNCHRONIZE are always granted to allow basic queries.
  if (rwx)
    mask |= READ_CONTROL | SYNCHRONIZE;
  return mask;
}

// Map rwx to kernel sync object rights (semaphores, events, mutants).
// The specific access bits 0x0001 and 0x0002 are QUERY_STATE and MODIFY_STATE
// respectively — identical across all three object types.
LIBC_INLINE ACCESS_MASK mode_bits_to_object_access_mask(int rwx) {
  ACCESS_MASK mask = 0;
  if (rwx & 4) // r → query state
    mask |= 0x0001;
  if (rwx & 2) // w → modify state (release/set/signal)
    mask |= 0x0002;
  if (rwx)
    mask |= READ_CONTROL | SYNCHRONIZE;
  return mask;
}

// Map rwx to section object rights (shared memory).
LIBC_INLINE ACCESS_MASK mode_bits_to_section_access_mask(int rwx) {
  ACCESS_MASK mask = 0;
  if (rwx & 4) // r
    mask |= SECTION_MAP_READ | SECTION_QUERY;
  if (rwx & 2) // w — include SECTION_EXTEND_SIZE so ftruncate/mmap can resize
    mask |= SECTION_MAP_WRITE | SECTION_EXTEND_SIZE;
  if (rwx & 1) // x
    mask |= SECTION_MAP_EXECUTE;
  if (rwx)
    mask |= READ_CONTROL | SYNCHRONIZE;
  return mask;
}

// Reverse: NT access mask to rwx bits (0-7).
LIBC_INLINE int access_mask_to_mode_bits(ACCESS_MASK mask) {
  int rwx = 0;
  if (mask & FILE_READ_DATA)
    rwx |= 4;
  if (mask & FILE_WRITE_DATA)
    rwx |= 2;
  if (mask & FILE_EXECUTE)
    rwx |= 1;
  return rwx;
}

//===----------------------------------------------------------------------===//
// Token SID queries
//===----------------------------------------------------------------------===//

// Retrieve the current process token's owner and primary group SIDs.
// Writes SID pointers into caller-provided stack buffers.
// owner_buf/group_buf must be at least 256 bytes each.
LIBC_INLINE NTSTATUS get_token_sids(UCHAR *owner_buf, ULONG owner_buf_size,
                                    SID **owner_out,
                                    UCHAR *group_buf, ULONG group_buf_size,
                                    SID **group_out) {
  HANDLE token = NtCurrentProcessToken();
  ULONG needed = 0;

  NTSTATUS status = ::NtQueryInformationToken(
      token, TokenUser, owner_buf, owner_buf_size, &needed);
  if (!NT_SUCCESS(status))
    return status;
  *owner_out = reinterpret_cast<TOKEN_USER *>(owner_buf)->User.Sid;

  status = ::NtQueryInformationToken(
      token, TokenPrimaryGroup, group_buf, group_buf_size, &needed);
  if (!NT_SUCCESS(status))
    return status;
  *group_out = reinterpret_cast<TOKEN_PRIMARY_GROUP *>(group_buf)->PrimaryGroup;

  return STATUS_SUCCESS;
}

//===----------------------------------------------------------------------===//
// DACL construction
//===----------------------------------------------------------------------===//

// Size of one ACCESS_ALLOWED_ACE with a given SID.
LIBC_INLINE ULONG ace_size(SID *sid) {
  // ACE_HEADER(4) + ACCESS_MASK(4) + SID bytes.
  // SidStart overlaps the first ULONG of the SID, but RtlLengthSid returns
  // the full SID size, so: sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) +
  // RtlLengthSid - sizeof(ULONG) ... but the standard layout is:
  //   offsetof(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid
  return static_cast<ULONG>(
      __builtin_offsetof(ACCESS_ALLOWED_ACE, SidStart) + ::RtlLengthSid(sid));
}

// Build a POSIX-style DACL with 3 ACEs (owner, group, everyone) into buf.
// buf must be large enough — 1024 bytes is always sufficient.
// Returns the total ACL size written, or 0 on failure.
// map_fn selects the rwx→ACCESS_MASK translation for the target object type.
LIBC_INLINE ULONG build_posix_dacl(UCHAR *buf, ULONG buf_size,
                                   mode_t mode, SID *owner, SID *group,
                                   AccessMaskFn map_fn = mode_bits_to_access_mask) {
  // Compute sizes.
  ULONG owner_ace_size = ace_size(owner);
  ULONG group_ace_size = ace_size(group);

  // Everyone SID: S-1-1-0 — 12 bytes (8 header + 1 SubAuthority).
  alignas(4) UCHAR everyone_buf[12];
  SID *everyone = reinterpret_cast<SID *>(everyone_buf);
  init_everyone_sid(everyone);
  ULONG everyone_ace_size = ace_size(everyone);

  ULONG acl_size = static_cast<ULONG>(sizeof(ACL)) +
                   owner_ace_size + group_ace_size + everyone_ace_size;
  if (acl_size > buf_size)
    return 0;

  // Initialize ACL header.
  ACL *acl = reinterpret_cast<ACL *>(buf);
  acl->AclRevision = ACL_REVISION;
  acl->Sbz1 = 0;
  acl->AclSize = static_cast<USHORT>(acl_size);
  acl->AceCount = 3;
  acl->Sbz2 = 0;

  // Helper to write one ACE.
  auto write_ace = [](UCHAR *dest, ACCESS_MASK mask, SID *sid,
                      ULONG total_ace_size) {
    ACCESS_ALLOWED_ACE *ace = reinterpret_cast<ACCESS_ALLOWED_ACE *>(dest);
    ace->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
    ace->Header.AceFlags = 0;
    ace->Header.AceSize = static_cast<USHORT>(total_ace_size);
    ace->Mask = mask;
    // Copy SID starting at SidStart.
    ::RtlCopySid(::RtlLengthSid(sid),
                 reinterpret_cast<SID *>(&ace->SidStart), sid);
  };

  UCHAR *ptr = buf + sizeof(ACL);
  // Owner always gets WRITE_DAC + FILE_WRITE_ATTRIBUTES + FILE_WRITE_EA so
  // chmod() can update the DACL, $LXMOD EA, and FILE_ATTRIBUTE_READONLY
  // regardless of the file's current mode. POSIX requires the owner to
  // always be able to chmod; these NT rights are the prerequisite.
  write_ace(ptr, map_fn((mode >> 6) & 7) | WRITE_DAC |
                     FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA,
            owner, owner_ace_size);
  ptr += owner_ace_size;
  write_ace(ptr, map_fn((mode >> 3) & 7), group, group_ace_size);
  ptr += group_ace_size;
  write_ace(ptr, map_fn(mode & 7), everyone, everyone_ace_size);

  return acl_size;
}

//===----------------------------------------------------------------------===//
// EA read/write for $LXMOD / $LXUID / $LXGID
//===----------------------------------------------------------------------===//

// Read $LXMOD EA from an open handle. Returns the mode or an error.
LIBC_INLINE ErrorOr<mode_t> read_ea_mode(HANDLE h) {
  // Build query for "$LXMOD".
  alignas(4) UCHAR query_buf[sizeof(FILE_GET_EA_INFORMATION) + 6]; // "$LXMOD"
  auto *query = reinterpret_cast<FILE_GET_EA_INFORMATION *>(query_buf);
  query->NextEntryOffset = 0;
  query->EaNameLength = 6;
  __builtin_memcpy(query->EaName, "$LXMOD", 7); // includes NUL

  alignas(4) UCHAR result_buf[sizeof(FILE_FULL_EA_INFORMATION) + 6 + 4];
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryEaFile(
      h, &iosb, result_buf, sizeof(result_buf),
      1,  // ReturnSingleEntry
      query_buf, sizeof(query_buf),
      nullptr, 1); // RestartScan

  if (!NT_SUCCESS(status))
    return Error(status == STATUS_ACCESS_DENIED ? EACCES : EIO);

  auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(result_buf);
  if (ea->EaValueLength < 4)
    return Error(EIO);

  // Value is a uint32_t immediately after the name + NUL. Offset is
  // EA_HDR + 6 + 1 = 15, not 4-byte aligned — memcpy handles this safely.
  UCHAR *value_ptr = result_buf +
      __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName) +
      ea->EaNameLength + 1;
  ULONG raw_mode;
  __builtin_memcpy(&raw_mode, value_ptr, 4);
  return static_cast<mode_t>(raw_mode);
}

// Read $LXUID EA from an open handle. Returns the uid or an error.
LIBC_INLINE ErrorOr<uid_t> read_ea_uid(HANDLE h) {
  alignas(4) UCHAR query_buf[sizeof(FILE_GET_EA_INFORMATION) + 6]; // "$LXUID"
  auto *query = reinterpret_cast<FILE_GET_EA_INFORMATION *>(query_buf);
  query->NextEntryOffset = 0;
  query->EaNameLength = 6;
  __builtin_memcpy(query->EaName, "$LXUID", 7);

  alignas(4) UCHAR result_buf[sizeof(FILE_FULL_EA_INFORMATION) + 6 + 4];
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryEaFile(
      h, &iosb, result_buf, sizeof(result_buf),
      1, query_buf, sizeof(query_buf), nullptr, 1);

  if (!NT_SUCCESS(status))
    return Error(status == STATUS_ACCESS_DENIED ? EACCES : EIO);

  auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(result_buf);
  if (ea->EaValueLength < 4)
    return Error(EIO);

  UCHAR *value_ptr = result_buf +
      __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName) +
      ea->EaNameLength + 1;
  ULONG raw_uid;
  __builtin_memcpy(&raw_uid, value_ptr, 4);
  return static_cast<uid_t>(raw_uid);
}

// Read $LXGID EA from an open handle. Returns the gid or an error.
LIBC_INLINE ErrorOr<gid_t> read_ea_gid(HANDLE h) {
  alignas(4) UCHAR query_buf[sizeof(FILE_GET_EA_INFORMATION) + 6]; // "$LXGID"
  auto *query = reinterpret_cast<FILE_GET_EA_INFORMATION *>(query_buf);
  query->NextEntryOffset = 0;
  query->EaNameLength = 6;
  __builtin_memcpy(query->EaName, "$LXGID", 7);

  alignas(4) UCHAR result_buf[sizeof(FILE_FULL_EA_INFORMATION) + 6 + 4];
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryEaFile(
      h, &iosb, result_buf, sizeof(result_buf),
      1, query_buf, sizeof(query_buf), nullptr, 1);

  if (!NT_SUCCESS(status))
    return Error(status == STATUS_ACCESS_DENIED ? EACCES : EIO);

  auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(result_buf);
  if (ea->EaValueLength < 4)
    return Error(EIO);

  UCHAR *value_ptr = result_buf +
      __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName) +
      ea->EaNameLength + 1;
  ULONG raw_gid;
  __builtin_memcpy(&raw_gid, value_ptr, 4);
  return static_cast<gid_t>(raw_gid);
}

// Write $LXMOD/$LXUID/$LXGID EAs. Pass (mode_t)-1 / (uid_t)-1 / (gid_t)-1
// to skip a field. Best-effort — caller should ignore EAS_NOT_SUPPORTED.
LIBC_INLINE NTSTATUS write_ea_mode_uid_gid(HANDLE h, mode_t mode,
                                           uid_t uid, gid_t gid) {
  // Each EA entry: header(8) + name(6) + NUL(1) + value(4) = 19 bytes.
  // ULONG-aligned = 20 bytes per entry, 3 entries max = 60 bytes.
  constexpr ULONG EA_HDR = static_cast<ULONG>(
      __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName));
  constexpr ULONG NAME_LEN = 6;  // "$LXMOD", "$LXUID", "$LXGID"
  constexpr ULONG VAL_LEN = 4;
  constexpr ULONG RAW_ENTRY = EA_HDR + NAME_LEN + 1 + VAL_LEN; // 19
  constexpr ULONG ALIGNED_ENTRY = (RAW_ENTRY + 3) & ~3u;        // 20

  alignas(4) UCHAR buf[ALIGNED_ENTRY * 3];
  ULONG count = 0;

  auto append = [&](const char *name, ULONG value) {
    ULONG base = count * ALIGNED_ENTRY;
    auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(buf + base);
    ea->NextEntryOffset = ALIGNED_ENTRY; // Patched for last entry below.
    ea->Flags = 0;
    ea->EaNameLength = static_cast<UCHAR>(NAME_LEN);
    ea->EaValueLength = static_cast<USHORT>(VAL_LEN);
    // Use offsetof-based pointers to avoid flex-array UB on EaName[1].
    UCHAR *name_dst = buf + base + EA_HDR;
    __builtin_memcpy(name_dst, name, NAME_LEN + 1);
    __builtin_memcpy(name_dst + NAME_LEN + 1, &value, VAL_LEN);
    count++;
  };

  if (mode != static_cast<mode_t>(-1))
    append("$LXMOD", static_cast<ULONG>(mode));
  if (uid != static_cast<uid_t>(-1))
    append("$LXUID", static_cast<ULONG>(uid));
  if (gid != static_cast<gid_t>(-1))
    append("$LXGID", static_cast<ULONG>(gid));

  if (count == 0)
    return STATUS_SUCCESS;

  // Last entry must have NextEntryOffset = 0.
  ULONG last_base = (count - 1) * ALIGNED_ENTRY;
  reinterpret_cast<FILE_FULL_EA_INFORMATION *>(buf + last_base)
      ->NextEntryOffset = 0;

  IO_STATUS_BLOCK iosb = {};
  return ::NtSetEaFile(h, &iosb, buf, count * ALIGNED_ENTRY);
}

//===----------------------------------------------------------------------===//
// Self-relative security descriptor parsing
//===----------------------------------------------------------------------===//

// NtQuerySecurityObject returns a self-relative SD with offsets.
struct SECURITY_DESCRIPTOR_RELATIVE {
  UCHAR Revision;
  UCHAR Sbz1;
  SECURITY_DESCRIPTOR_CONTROL Control;
  ULONG OffsetOwner;
  ULONG OffsetGroup;
  ULONG OffsetSacl;
  ULONG OffsetDacl;
};

// Parse a self-relative SD buffer into pointers. Returns false on error.
struct ParsedSD {
  SID *owner;
  SID *group;
  ACL *dacl;
};

LIBC_INLINE bool parse_self_relative_sd(UCHAR *sd_buf, ParsedSD *out) {
  auto *rel = reinterpret_cast<SECURITY_DESCRIPTOR_RELATIVE *>(sd_buf);
  if (!(rel->Control & SE_SELF_RELATIVE))
    return false;
  out->owner = rel->OffsetOwner
      ? reinterpret_cast<SID *>(sd_buf + rel->OffsetOwner) : nullptr;
  out->group = rel->OffsetGroup
      ? reinterpret_cast<SID *>(sd_buf + rel->OffsetGroup) : nullptr;
  out->dacl = (rel->Control & SE_DACL_PRESENT) && rel->OffsetDacl
      ? reinterpret_cast<ACL *>(sd_buf + rel->OffsetDacl) : nullptr;
  return true;
}

//===----------------------------------------------------------------------===//
// DACL reading — reverse-map to POSIX mode bits
//===----------------------------------------------------------------------===//

// Read the DACL from a handle and reconstruct owner/group/other mode bits.
// Returns the 9-bit permission portion (no file type bits).
LIBC_INLINE ErrorOr<mode_t> read_dacl_mode(HANDLE h) {
  auto sd_s = internal::byte_scratch(1024);
  if (!sd_s)
    return Error(ENOMEM);
  auto *sd_buf = reinterpret_cast<UCHAR *>(sd_s.data());
  ULONG needed = 0;
  NTSTATUS status = ::NtQuerySecurityObject(
      h, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
         DACL_SECURITY_INFORMATION,
      reinterpret_cast<SECURITY_DESCRIPTOR *>(sd_buf),
      static_cast<ULONG>(sd_s.size()), &needed);
  if (!NT_SUCCESS(status))
    return Error(status == STATUS_ACCESS_DENIED ? EACCES : EIO);

  ParsedSD sd;
  if (!parse_self_relative_sd(sd_buf, &sd))
    return Error(EIO);
  if (!sd.dacl)
    return static_cast<mode_t>(0777); // No DACL = full access.

  // Build the Everyone SID for comparison.
  alignas(4) UCHAR everyone_buf[12];
  SID *everyone = reinterpret_cast<SID *>(everyone_buf);
  init_everyone_sid(everyone);

  // Walk ACEs and accumulate granted/denied rights per class.
  ACCESS_MASK owner_allow = 0, group_allow = 0, other_allow = 0;
  ACCESS_MASK owner_deny = 0, group_deny = 0, other_deny = 0;

  UCHAR *ace_ptr = reinterpret_cast<UCHAR *>(sd.dacl) + sizeof(ACL);
  for (USHORT i = 0; i < sd.dacl->AceCount; i++) {
    ACE_HEADER *hdr = reinterpret_cast<ACE_HEADER *>(ace_ptr);
    if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE ||
        hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
      auto *ace = reinterpret_cast<ACCESS_ALLOWED_ACE *>(ace_ptr);
      SID *ace_sid = reinterpret_cast<SID *>(&ace->SidStart);

      bool is_owner = sd.owner && ::RtlEqualSid(ace_sid, sd.owner);
      bool is_group = sd.group && ::RtlEqualSid(ace_sid, sd.group);
      bool is_everyone = ::RtlEqualSid(ace_sid, everyone);

      if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        if (is_owner) owner_allow |= ace->Mask;
        if (is_group) group_allow |= ace->Mask;
        if (is_everyone) other_allow |= ace->Mask;
      } else {
        if (is_owner) owner_deny |= ace->Mask;
        if (is_group) group_deny |= ace->Mask;
        if (is_everyone) other_deny |= ace->Mask;
      }
    }
    ace_ptr += hdr->AceSize;
  }

  int owner_bits = access_mask_to_mode_bits(owner_allow & ~owner_deny);
  int group_bits = access_mask_to_mode_bits(group_allow & ~group_deny);
  int other_bits = access_mask_to_mode_bits(other_allow & ~other_deny);

  return static_cast<mode_t>((owner_bits << 6) | (group_bits << 3) |
                             other_bits);
}

//===----------------------------------------------------------------------===//
// Atomic creation security descriptor
//===----------------------------------------------------------------------===//

// Note on special mode bits stored in $LXMOD EAs:
//
//   S_ISUID (04000): stored and reported by stat, NOT enforced at exec
//     time. See execve.cpp for rationale and future-work plan.
//   S_ISGID (02000): stored, reported, AND enforced — new files in a
//     setgid directory inherit the directory's group (query_parent_sgid).
//   S_ISVTX (01000): stored, reported, AND enforced — deletion in a
//     sticky directory requires ownership (check_sticky_bit).

// Stack buffer large enough for an absolute-form SECURITY_DESCRIPTOR +
// a 3-ACE ACL with typical SIDs (owner ~28B, group ~28B, everyone 12B).
// sizeof(SD)=40 + sizeof(ACL)=8 + 3*(8+28)=108 = 156. Round up.
inline constexpr ULONG CREATION_SD_BUF_SIZE = 512;

// Query a parent directory's S_ISGID status and group. Defined below.
LIBC_INLINE bool query_parent_sgid(HANDLE parent_handle, gid_t *parent_gid,
                                   SID *parent_gsid_out);

// Build a SECURITY_DESCRIPTOR with a POSIX-mode DACL for use in
// OBJECT_ATTRIBUTES.SecurityDescriptor during NtCreateFile. The file is
// born with the correct DACL — no race window, no WRITE_DAC needed.
//
// If parent_handle is non-null, checks the parent directory for S_ISGID.
// When set, the new file's group ACE uses the parent's group instead of
// the creator's primary group, and S_ISGID is propagated to mode for
// new subdirectories.
//
// sd_buf must be at least CREATION_SD_BUF_SIZE bytes, 8-byte aligned.
// On success, returns a pointer to the SECURITY_DESCRIPTOR in sd_buf.
// On failure (e.g., cannot query token SIDs), returns nullptr — caller
// should proceed without a SecurityDescriptor (inherits parent DACL).
//
// *effective_mode_out receives the final mode (with S_ISGID propagated
// if applicable). Pass nullptr to ignore.
LIBC_INLINE SECURITY_DESCRIPTOR *build_creation_sd(
    UCHAR *sd_buf, mode_t mode, HANDLE parent_handle = nullptr,
    mode_t *effective_mode_out = nullptr,
    AccessMaskFn map_fn = mode_bits_to_access_mask) {
  mode &= 07777;

  // Query owner and group SIDs from the process token.
  auto sid_s = internal::byte_scratch(512);
  if (!sid_s)
    return nullptr;
  UCHAR *owner_buf = reinterpret_cast<UCHAR *>(sid_s.data());
  UCHAR *group_buf = owner_buf + 256;
  SID *owner_sid = nullptr;
  SID *group_sid = nullptr;
  NTSTATUS status = get_token_sids(owner_buf, 256u, &owner_sid,
                                   group_buf, 256u, &group_sid);
  if (!NT_SUCCESS(status))
    return nullptr;

  // S_ISGID inheritance: if the parent directory has S_ISGID, use its
  // group SID instead of the creator's primary group.
  alignas(4) UCHAR parent_gsid_buf[MAX_SID_SIZE];
  SID *parent_gsid = reinterpret_cast<SID *>(parent_gsid_buf);
  gid_t parent_gid = 0;
  if (parent_handle && query_parent_sgid(parent_handle, &parent_gid,
                                          parent_gsid)) {
    group_sid = parent_gsid;
    // Propagate S_ISGID to new directories (not files — POSIX semantics).
    // The caller (mkdirat) includes S_ISGID in its mode; open() does not.
  }

  // Build the ACL after the SD in the same buffer.
  SECURITY_DESCRIPTOR *sd = reinterpret_cast<SECURITY_DESCRIPTOR *>(sd_buf);
  UCHAR *acl_start = sd_buf + sizeof(SECURITY_DESCRIPTOR);
  ULONG acl_buf_size = CREATION_SD_BUF_SIZE -
                       static_cast<ULONG>(sizeof(SECURITY_DESCRIPTOR));
  ULONG acl_size = build_posix_dacl(acl_start, acl_buf_size,
                                    mode, owner_sid, group_sid, map_fn);
  if (acl_size == 0)
    return nullptr;

  sd->Revision = SECURITY_DESCRIPTOR_REVISION;
  sd->Sbz1 = 0;
  sd->Control = SE_DACL_PRESENT | SE_DACL_PROTECTED;
  sd->Owner = nullptr;
  sd->Group = nullptr;
  sd->Sacl = nullptr;
  sd->Dacl = reinterpret_cast<ACL *>(acl_start);

  if (effective_mode_out)
    *effective_mode_out = mode;

  return sd;
}

//===----------------------------------------------------------------------===//
// Post-creation permission fixup (EA + READONLY only)
//===----------------------------------------------------------------------===//

// Write $LXMOD/$LXUID/$LXGID EAs and sync FILE_ATTRIBUTE_READONLY after
// file creation. Always stamps the creator's euid; uses inherited_gid when
// S_ISGID inheritance applies, otherwise stamps the creator's egid.
// The DACL was already set atomically via OBJECT_ATTRIBUTES.SecurityDescriptor.
// Best-effort — returns 0 always.
LIBC_INLINE int post_create_perms(HANDLE h, mode_t mode,
                                   gid_t inherited_gid = static_cast<gid_t>(-1)) {
  mode &= 07777;

  // Stamp owner: always the effective uid of the creating process.
  uid_t owner_uid = static_cast<uid_t>(
      g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED));
  // Stamp group: inherited_gid if S_ISGID on parent, else effective gid.
  gid_t owner_gid = (inherited_gid != static_cast<gid_t>(-1))
                         ? inherited_gid
                         : static_cast<gid_t>(
                               g_pcb.identity.eff_gid.load(
                                   cpp::MemoryOrder::RELAXED));

  // Tier 3: Write $LXMOD + $LXUID + $LXGID EAs.
  write_ea_mode_uid_gid(h, mode, owner_uid, owner_gid);

  // Tier 1: Sync FILE_ATTRIBUTE_READONLY.
  IO_STATUS_BLOCK iosb = {};
  FILE_BASIC_INFORMATION basic = {};
  NTSTATUS status = ::NtQueryInformationFile(h, &iosb, &basic, sizeof(basic),
                                             FileBasicInformation);
  if (NT_SUCCESS(status)) {
    if (mode & 0222)
      basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    else
      basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    if (basic.FileAttributes == 0)
      basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    basic.CreationTime.QuadPart = 0;
    basic.LastAccessTime.QuadPart = 0;
    basic.LastWriteTime.QuadPart = 0;
    basic.ChangeTime.QuadPart = 0;
    ::NtSetInformationFile(h, &iosb, &basic, sizeof(basic),
                           FileBasicInformation);
  }

  return 0;
}

//===----------------------------------------------------------------------===//
// Handle re-open for access escalation
//===----------------------------------------------------------------------===//

// Re-open a file handle with additional access rights. Uses empty ObjectName
// relative to the existing handle — the kernel reopens the same file object
// with a fresh access check against the file's DACL. One syscall, no path
// resolution. Returns nullptr on failure.
LIBC_INLINE HANDLE reopen_with_access(HANDLE h, ACCESS_MASK access) {
  windows::nt_wstring_view empty;

  auto oa = windows::named_internal_oa(&empty, h);

  HANDLE new_handle = nullptr;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      &new_handle, access, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_OPEN_FOR_BACKUP_INTENT);
  return NT_SUCCESS(status) ? new_handle : nullptr;
}

//===----------------------------------------------------------------------===//
// chmod core implementation
//===----------------------------------------------------------------------===//

// Apply a POSIX mode to an open handle. Three-tier: DACL + EA + READONLY.
//
// If the handle lacks WRITE_DAC (e.g., fchmod on a read-only fd), reopens
// the same file with the needed access. POSIX requires fchmod to succeed
// if the caller owns the file, regardless of how the fd was opened.
//
// Returns 0 on success, errno on failure.
LIBC_INLINE int chmod_impl(HANDLE h, mode_t mode) {
  mode &= 07777;

  // Get current user's owner and group SIDs.
  auto sid_s = internal::byte_scratch(512);
  if (!sid_s)
    return ENOMEM;
  UCHAR *owner_buf = reinterpret_cast<UCHAR *>(sid_s.data());
  UCHAR *group_buf = owner_buf + 256;
  SID *owner_sid = nullptr;
  SID *group_sid = nullptr;
  NTSTATUS status = get_token_sids(owner_buf, 256u, &owner_sid,
                                   group_buf, 256u, &group_sid);
  if (!NT_SUCCESS(status))
    return EIO;

  // Tier 2: Build and apply DACL.
  auto acl_s = internal::byte_scratch(1024);
  if (!acl_s)
    return ENOMEM;
  UCHAR *acl_buf = reinterpret_cast<UCHAR *>(acl_s.data());
  ULONG acl_size = build_posix_dacl(acl_buf, static_cast<ULONG>(acl_s.size()),
                                    mode, owner_sid, group_sid);

  windows::ScopedNtHandle dacl_handle; // Non-null if we reopened for WRITE_DAC.

  if (acl_size > 0) {
    SECURITY_DESCRIPTOR sd;
    sd.Revision = SECURITY_DESCRIPTOR_REVISION;
    sd.Sbz1 = 0;
    sd.Control = SE_DACL_PRESENT | SE_DACL_PROTECTED;
    sd.Owner = nullptr;
    sd.Group = nullptr;
    sd.Sacl = nullptr;
    sd.Dacl = reinterpret_cast<ACL *>(acl_buf);

    status = ::NtSetSecurityObject(h, DACL_SECURITY_INFORMATION, &sd);

    if (status == STATUS_ACCESS_DENIED) {
      // Handle lacks WRITE_DAC — reopen the same file with it.
      // Owner always has implicit WRITE_DAC, so the reopen succeeds
      // if the caller actually owns the file.
      dacl_handle.reset(reopen_with_access(h, WRITE_DAC));
      if (dacl_handle) {
        ::NtSetSecurityObject(dacl_handle.get(), DACL_SECURITY_INFORMATION, &sd);
      }
    }
    // Ignore STATUS_NOT_SUPPORTED / STATUS_INVALID_DEVICE_REQUEST
    // (filesystem doesn't support DACLs — FAT32, some SMB).
  }

  // Tier 3: Write $LXMOD EA (best-effort).
  // The owner ACE always includes FILE_WRITE_EA, so h has the needed access.
  write_ea_mode_uid_gid(h, mode, static_cast<uid_t>(-1),
                        static_cast<gid_t>(-1));

  // Tier 1: Sync FILE_ATTRIBUTE_READONLY.
  // The owner ACE always includes FILE_READ/WRITE_ATTRIBUTES.
  IO_STATUS_BLOCK iosb = {};
  FILE_BASIC_INFORMATION basic = {};
  status = ::NtQueryInformationFile(h, &iosb, &basic, sizeof(basic),
                                   FileBasicInformation);
  if (NT_SUCCESS(status)) {
    if (mode & 0222)
      basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    else
      basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    if (basic.FileAttributes == 0)
      basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    basic.CreationTime.QuadPart = 0;
    basic.LastAccessTime.QuadPart = 0;
    basic.LastWriteTime.QuadPart = 0;
    basic.ChangeTime.QuadPart = 0;
    ::NtSetInformationFile(h, &iosb, &basic, sizeof(basic),
                          FileBasicInformation);
  }

  return 0;
}

//===----------------------------------------------------------------------===//
// chown core implementation
//===----------------------------------------------------------------------===//

// Change ownership/group of an open handle. Returns 0 or errno.
//
// Owner change: attempts NtSetSecurityObject(OWNER_SECURITY_INFORMATION).
// Requires SeTakeOwnershipPrivilege or SeRestorePrivilege for non-self
// changes. Let NT enforce privilege — returns EPERM on failure.
//
// Group change: rebuilds the DACL with the current mode but the file's SD
// group SID updated. Only needs WRITE_DAC (file owners always have this).
//
// Both record uid/gid via $LXUID/$LXGID EAs for bit-perfect round-trip.
LIBC_INLINE int chown_impl(HANDLE h, uid_t uid, gid_t gid) {
  windows::ScopedNtHandle work_handle;

  // Read current security descriptor.
  auto sd_s = internal::byte_scratch(1024);
  if (!sd_s)
    return ENOMEM;
  auto *sd_buf = reinterpret_cast<UCHAR *>(sd_s.data());
  ULONG needed = 0;
  NTSTATUS status = ::NtQuerySecurityObject(
      h, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
         DACL_SECURITY_INFORMATION,
      reinterpret_cast<SECURITY_DESCRIPTOR *>(sd_buf),
      static_cast<ULONG>(sd_s.size()), &needed);

  // Reopen if handle lacks READ_CONTROL (POSIX: fchown works on any fd).
  if (status == STATUS_ACCESS_DENIED) {
    work_handle.reset(reopen_with_access(
        h, READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_WRITE_EA));
    if (work_handle) {
      h = work_handle.get();
      status = ::NtQuerySecurityObject(
          h, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
             DACL_SECURITY_INFORMATION,
          reinterpret_cast<SECURITY_DESCRIPTOR *>(sd_buf),
          static_cast<ULONG>(sd_s.size()), &needed);
    }
  }

  ParsedSD current;
  current.owner = nullptr;
  current.group = nullptr;
  current.dacl = nullptr;
  if (NT_SUCCESS(status))
    parse_self_relative_sd(sd_buf, &current);

  int result = 0;

  // --- Owner change ---
  if (uid != static_cast<uid_t>(-1)) {
    uid_t current_uid = current.owner ? sid_to_uid(current.owner) : 0;
    if (uid != current_uid) {
      // Build target SID from uid using the domain prefix.
      alignas(4) UCHAR new_sid_buf[MAX_SID_SIZE];
      SID *new_sid = reinterpret_cast<SID *>(new_sid_buf);
      if (uid_to_sid(uid, new_sid)) {
        SECURITY_DESCRIPTOR owner_sd;
        owner_sd.Revision = SECURITY_DESCRIPTOR_REVISION;
        owner_sd.Sbz1 = 0;
        owner_sd.Control = 0;
        owner_sd.Owner = new_sid;
        owner_sd.Group = nullptr;
        owner_sd.Sacl = nullptr;
        owner_sd.Dacl = nullptr;

        status = ::NtSetSecurityObject(
            h, OWNER_SECURITY_INFORMATION, &owner_sd);
        if (status == STATUS_PRIVILEGE_NOT_HELD ||
            status == STATUS_ACCESS_DENIED)
          result = EPERM;
        // STATUS_NOT_SUPPORTED → filesystem doesn't support ownership.
      }
    }
  }

  // --- Group change ---
  if (result == 0 && gid != static_cast<gid_t>(-1)) {
    gid_t current_gid = current.group ? sid_to_uid(current.group) : 0;
    if (gid != current_gid) {
      // Build target group SID and set it in the SD.
      alignas(4) UCHAR new_gsid_buf[MAX_SID_SIZE];
      SID *new_gsid = reinterpret_cast<SID *>(new_gsid_buf);
      if (uid_to_sid(static_cast<uid_t>(gid), new_gsid)) {
        SECURITY_DESCRIPTOR group_sd;
        group_sd.Revision = SECURITY_DESCRIPTOR_REVISION;
        group_sd.Sbz1 = 0;
        group_sd.Control = 0;
        group_sd.Owner = nullptr;
        group_sd.Group = new_gsid;
        group_sd.Sacl = nullptr;
        group_sd.Dacl = nullptr;

        status = ::NtSetSecurityObject(
            h, GROUP_SECURITY_INFORMATION, &group_sd);
        if (status == STATUS_PRIVILEGE_NOT_HELD ||
            status == STATUS_ACCESS_DENIED)
          result = EPERM;
      }
    }
  }

  // Record uid/gid via EAs for bit-perfect stat() round-trip.
  write_ea_mode_uid_gid(h, static_cast<mode_t>(-1), uid, gid);

  return result;
}

//===----------------------------------------------------------------------===//
// Sticky bit (S_ISVTX) enforcement for unlink
//===----------------------------------------------------------------------===//

// Check whether the caller is permitted to delete a file in a sticky
// directory. Returns 0 if allowed, EPERM if not.
// parent_handle: open handle to the parent directory.
// file_handle: open handle to the file being deleted.
LIBC_INLINE int check_sticky_bit(HANDLE parent_handle, HANDLE file_handle) {
  // Read parent's mode from EA. If no EA or no sticky bit, allow.
  auto parent_mode = read_ea_mode(parent_handle);
  if (!parent_mode.has_value() || !(parent_mode.value() & S_ISVTX))
    return 0;

  // Sticky bit is set. Deletion is allowed only if the caller is:
  //   1. The file's owner, OR
  //   2. The directory's owner, OR
  //   3. Root (uid 0 / Administrators).
  auto sid_s = internal::byte_scratch(512);
  if (!sid_s)
    return 0; // Can't determine — allow (fail-open).
  auto *caller_buf = reinterpret_cast<UCHAR *>(sid_s.data());
  SID *caller_sid = nullptr;
  SID *dummy = nullptr;
  auto *dummy_buf = caller_buf + 256;
  NTSTATUS status = get_token_sids(caller_buf, 256u,
                                   &caller_sid, dummy_buf,
                                   256u, &dummy);
  if (!NT_SUCCESS(status))
    return 0; // Can't determine — allow (fail-open for robustness).

  uid_t caller_uid = sid_to_uid(caller_sid);
  if (caller_uid == 0)
    return 0; // Root equivalent.

  // Get file owner.
  auto sdq_s = internal::byte_scratch(1024);
  if (!sdq_s)
    return 0; // Can't determine — allow (fail-open).
  auto *file_sd_buf = reinterpret_cast<UCHAR *>(sdq_s.data());
  auto *dir_sd_buf = file_sd_buf + 512;
  ULONG needed = 0;
  status = ::NtQuerySecurityObject(
      file_handle, OWNER_SECURITY_INFORMATION,
      reinterpret_cast<SECURITY_DESCRIPTOR *>(file_sd_buf),
      512u, &needed);
  if (NT_SUCCESS(status)) {
    ParsedSD file_sd;
    if (parse_self_relative_sd(file_sd_buf, &file_sd) && file_sd.owner) {
      if (::RtlEqualSid(caller_sid, file_sd.owner))
        return 0; // Caller owns the file.
    }
  }

  // Get directory owner.
  status = ::NtQuerySecurityObject(
      parent_handle, OWNER_SECURITY_INFORMATION,
      reinterpret_cast<SECURITY_DESCRIPTOR *>(dir_sd_buf),
      512u, &needed);
  if (NT_SUCCESS(status)) {
    ParsedSD dir_sd;
    if (parse_self_relative_sd(dir_sd_buf, &dir_sd) && dir_sd.owner) {
      if (::RtlEqualSid(caller_sid, dir_sd.owner))
        return 0; // Caller owns the directory.
    }
  }

  return EPERM;
}

//===----------------------------------------------------------------------===//
// S_ISGID directory group inheritance query
//===----------------------------------------------------------------------===//

// Query a parent directory's S_ISGID status and group. If the parent has
// S_ISGID set, returns the parent's group SID and gid via out parameters.
// Returns true if S_ISGID is active (caller should use parent_gid/parent_gsid
// instead of the creator's primary group).
LIBC_INLINE bool query_parent_sgid(HANDLE parent_handle, gid_t *parent_gid,
                                   SID *parent_gsid_out) {
  // Check parent's $LXMOD for S_ISGID.
  auto parent_mode = read_ea_mode(parent_handle);
  if (!parent_mode.has_value() || !(parent_mode.value() & S_ISGID))
    return false;

  // Read parent's group from $LXGID EA.
  alignas(4) UCHAR query_buf[sizeof(FILE_GET_EA_INFORMATION) + 6];
  auto *query = reinterpret_cast<FILE_GET_EA_INFORMATION *>(query_buf);
  query->NextEntryOffset = 0;
  query->EaNameLength = 6;
  __builtin_memcpy(query->EaName, "$LXGID", 7);

  alignas(4) UCHAR result_buf[sizeof(FILE_FULL_EA_INFORMATION) + 6 + 4];
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryEaFile(
      parent_handle, &iosb, result_buf, sizeof(result_buf),
      1, query_buf, sizeof(query_buf), nullptr, 1);

  if (!NT_SUCCESS(status))
    return false;

  auto *ea = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(result_buf);
  if (ea->EaValueLength < 4)
    return false;

  UCHAR *val = result_buf +
      __builtin_offsetof(FILE_FULL_EA_INFORMATION, EaName) +
      ea->EaNameLength + 1;
  ULONG raw_gid;
  __builtin_memcpy(&raw_gid, val, 4);
  *parent_gid = static_cast<gid_t>(raw_gid);

  // Build the parent's group SID from the gid.
  uid_to_sid(static_cast<uid_t>(raw_gid), parent_gsid_out);
  return true;
}

//===----------------------------------------------------------------------===//
// Hardened IPC directory creation
//===----------------------------------------------------------------------===//

// Create an IPC directory (e.g., llvm_sem, llvm_shm) with owner-only DACL
// and verify it hasn't been tampered with if it already exists.
//
// Three defenses:
//   1. Owner-only DACL (0700) prevents other users from creating/reading
//      marker files in the directory.
//   2. FILE_OPEN_REPARSE_POINT opens the entry itself — if it's a junction
//      or symlink planted by an attacker, we detect it and refuse.
//   3. Ownership verification on pre-existing directories blocks pre-creation
//      attacks where an attacker creates the directory before the victim.
//
// nt_path/nt_path_len: full NT path (e.g., \??\C:\Users\...\llvm_sem).
// Returns true if the directory is safe to use.
LIBC_INLINE bool ensure_secure_ipc_dir(WCHAR *nt_path, size_t nt_path_len) {
  windows::nt_wstring_view path_wsv(nt_path, nt_path_len);

  OBJECT_ATTRIBUTES oa;
  __builtin_memset(&oa, 0, sizeof(oa));
  oa.Length = sizeof(oa);
  oa.ObjectName = path_wsv.unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  // Owner-only DACL.
  auto sd_s = internal::byte_scratch(CREATION_SD_BUF_SIZE);
  if (!sd_s)
    return false;
  oa.SecurityDescriptor =
      build_creation_sd(reinterpret_cast<UCHAR *>(sd_s.data()), 0700);

  IO_STATUS_BLOCK iosb = {};
  windows::ScopedNtHandle dir_h;
  // FILE_OPEN_IF: create if absent, open if present.
  // FILE_OPEN_REPARSE_POINT: open the directory entry itself so we can
  // detect junctions/symlinks rather than silently following them.
  NTSTATUS st = ::NtCreateFile(
      dir_h.put(),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
      &oa, &iosb, nullptr,
      FILE_ATTRIBUTE_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_OPEN_IF,
      FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
          FILE_OPEN_REPARSE_POINT,
      nullptr, 0);
  if (!NT_SUCCESS(st))
    return false;

  bool safe = true;

  // Check for reparse point (junction/symlink).
  FILE_BASIC_INFORMATION basic = {};
  IO_STATUS_BLOCK basic_iosb = {};
  st = ::NtQueryInformationFile(dir_h.get(), &basic_iosb, &basic, sizeof(basic),
                                 FileBasicInformation);
  if (NT_SUCCESS(st) &&
      (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    safe = false;

  // For pre-existing directories, verify the owner is us.
  if (safe && iosb.Information != FILE_CREATED_RESULT) {
    alignas(8) UCHAR query_buf[256];
    ULONG needed = 0;
    st = ::NtQuerySecurityObject(
        dir_h.get(), OWNER_SECURITY_INFORMATION,
        reinterpret_cast<SECURITY_DESCRIPTOR *>(query_buf),
        sizeof(query_buf), &needed);
    if (NT_SUCCESS(st)) {
      ParsedSD parsed = {};
      if (parse_self_relative_sd(query_buf, &parsed) && parsed.owner) {
        alignas(8) UCHAR tok_owner[256], tok_group[256];
        SID *our_owner = nullptr;
        SID *our_group = nullptr;
        if (NT_SUCCESS(get_token_sids(tok_owner, sizeof(tok_owner), &our_owner,
                                       tok_group, sizeof(tok_group),
                                       &our_group))) {
          if (!::RtlEqualSid(parsed.owner, our_owner))
            safe = false;
        }
      }
    } else {
      // Can't verify ownership — treat as unsafe.
      safe = false;
    }
  }

  return safe;
}

} // namespace windows_sec
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_H
