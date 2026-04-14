//===-- NT file info to struct stat conversion -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared conversion logic for stat(), fstat(), lstat().
//
// NT times are 100ns intervals since 1601-01-01.
// Unix times are seconds + nanoseconds since 1970-01-01.
// Delta: 11644473600 seconds (369 years).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_NT_TO_STAT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_NT_TO_STAT_H

#include "src/__support/OSUtil/windows/ipc/fifo.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "include/llvm-libc-types/struct_stat.h"
#include "hdr/sys_stat_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// 100ns ticks between 1601-01-01 and 1970-01-01.
inline constexpr LONGLONG NT_EPOCH_DELTA = 116444736000000000LL;
inline constexpr LONGLONG TICKS_PER_SECOND = 10000000LL;
inline constexpr LONGLONG NS_PER_TICK = 100LL;

LIBC_INLINE struct timespec filetime_to_timespec(LARGE_INTEGER ft) {
  struct timespec ts;
  LONGLONG ticks = ft.QuadPart - NT_EPOCH_DELTA;
  // Euclidean division: tv_nsec must be non-negative per POSIX, even for
  // pre-1970 timestamps where ticks is negative. C++ truncation toward zero
  // would produce negative remainders, so adjust floor-ward.
  LONGLONG q = ticks / TICKS_PER_SECOND;
  LONGLONG r = ticks % TICKS_PER_SECOND;
  if (r < 0) {
    --q;
    r += TICKS_PER_SECOND;
  }
  ts.tv_sec = static_cast<time_t>(q);
  ts.tv_nsec = static_cast<long>(r * NS_PER_TICK);
  return ts;
}

// Synthesize POSIX st_mode from NT file attributes, reparse tag, and
// device type. device_type comes from FILE_STAT_BASIC_INFORMATION.DeviceType.
LIBC_INLINE mode_t nt_attributes_to_mode(ULONG attrs, ULONG reparse_tag,
                                          ULONG device_type) {
  mode_t mode = 0;

  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
      reparse_tag == IO_REPARSE_TAG_SYMLINK)
    mode |= S_IFLNK;
  else if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    mode |= S_IFDIR;
  else if (device_type == FILE_DEVICE_NAMED_PIPE)
    mode |= S_IFIFO;
  else
    mode |= S_IFREG;

  // Windows has no owner/group/other distinction. Synthesize from
  // read-only attribute: read-only → r--r--r--, otherwise rwxrwxrwx.
  if (attrs & FILE_ATTRIBUTE_READONLY)
    mode |= S_IRUSR | S_IRGRP | S_IROTH;
  else
    mode |= S_IRWXU | S_IRWXG | S_IRWXO;

  return mode;
}

// Fill struct stat from FILE_STAT_BASIC_INFORMATION.
LIBC_INLINE void
nt_stat_basic_to_stat(const FILE_STAT_BASIC_INFORMATION &info,
                      struct stat *out) {
  out->st_dev = static_cast<dev_t>(info.VolumeSerialNumber.QuadPart);
  out->st_ino = static_cast<ino_t>(info.FileId.QuadPart);
  out->st_mode = nt_attributes_to_mode(info.FileAttributes, info.ReparseTag,
                                        info.DeviceType);
  out->st_nlink = static_cast<nlink_t>(info.NumberOfLinks);
  out->st_uid = 0;
  out->st_gid = 0;
  out->st_rdev = 0;
  out->st_size = static_cast<off_t>(info.EndOfFile.QuadPart);
  out->st_atim = filetime_to_timespec(info.LastAccessTime);
  out->st_mtim = filetime_to_timespec(info.LastWriteTime);
  out->st_ctim = filetime_to_timespec(info.ChangeTime);
  out->st_blksize = 4096;
  out->st_blocks =
      static_cast<blkcnt_t>((info.AllocationSize.QuadPart + 511) / 512);
}

// Fixup st_mode for FIFO marker files detected by path-based stat.
// NtQueryInformationByName reports disk-file DeviceType for marker files,
// so nt_attributes_to_mode doesn't catch them. This opens the file briefly
// to read the magic bytes. Only called when the candidate filter matches.
LIBC_INLINE void fixup_fifo_mode_impl(OBJECT_ATTRIBUTES *oa,
                                       ULONG attrs, LONGLONG file_size,
                                       struct stat *out) {
  if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    return;
  if (!(attrs & FILE_ATTRIBUTE_SYSTEM))
    return;
  if (file_size < 8 ||
      file_size > static_cast<LONGLONG>(FIFO_MARKER_SIZE))
    return;

  HANDLE h;
  IO_STATUS_BLOCK open_iosb = {};
  NTSTATUS st = NtOpenFile(
      &h, FILE_READ_DATA | SYNCHRONIZE, oa, &open_iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(st))
    return;

  if (is_fifo_marker(h)) {
    out->st_mode = (out->st_mode & ~static_cast<mode_t>(S_IFMT)) | S_IFIFO;
    out->st_size = 0;
  }
  NtClose(h);
}

// Overload for FILE_STAT_BASIC_INFORMATION.
LIBC_INLINE void fixup_fifo_mode(OBJECT_ATTRIBUTES *oa,
                                  const FILE_STAT_BASIC_INFORMATION &info,
                                  struct stat *out) {
  fixup_fifo_mode_impl(oa, info.FileAttributes, info.EndOfFile.QuadPart, out);
}

// Overload for FILE_STAT_LX_INFORMATION.
LIBC_INLINE void fixup_fifo_mode(OBJECT_ATTRIBUTES *oa,
                                  const FILE_STAT_LX_INFORMATION &info,
                                  struct stat *out) {
  fixup_fifo_mode_impl(oa, info.FileAttributes, info.EndOfFile.QuadPart, out);
}

// Fill struct stat from FILE_STAT_LX_INFORMATION. Uses LxMode/LxUid/LxGid
// when WSL metadata is present, otherwise falls back to attribute-based mode.
// st_dev is zero — FILE_STAT_LX_INFORMATION lacks VolumeSerialNumber.
// Callers with a HANDLE (fstat) should fill st_dev via query_volume_serial().
LIBC_INLINE void
nt_stat_lx_to_stat(const FILE_STAT_LX_INFORMATION &info, struct stat *out) {
  out->st_dev = 0;
  out->st_ino = static_cast<ino_t>(info.FileId.QuadPart);
  out->st_nlink = static_cast<nlink_t>(info.NumberOfLinks);
  out->st_rdev = 0;
  out->st_size = static_cast<off_t>(info.EndOfFile.QuadPart);
  out->st_atim = filetime_to_timespec(info.LastAccessTime);
  out->st_mtim = filetime_to_timespec(info.LastWriteTime);
  out->st_ctim = filetime_to_timespec(info.ChangeTime);
  out->st_blksize = 4096;
  out->st_blocks =
      static_cast<blkcnt_t>((info.AllocationSize.QuadPart + 511) / 512);

  if (info.LxFlags & LX_FILE_METADATA_HAS_MODE) {
    // Tier 3: Exact POSIX mode from WSL EA metadata.
    mode_t type_bits = 0;
    if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        info.ReparseTag == IO_REPARSE_TAG_SYMLINK)
      type_bits = S_IFLNK;
    else if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      type_bits = S_IFDIR;
    else
      type_bits = S_IFREG;
    out->st_mode = type_bits | (static_cast<mode_t>(info.LxMode) & 07777);
  } else {
    // Tier 1 fallback: binary mode from FILE_ATTRIBUTE_READONLY.
    out->st_mode = nt_attributes_to_mode(info.FileAttributes,
                                          info.ReparseTag, 0);
  }

  out->st_uid = (info.LxFlags & LX_FILE_METADATA_HAS_UID)
                    ? static_cast<uid_t>(info.LxUid)
                    : 0;
  out->st_gid = (info.LxFlags & LX_FILE_METADATA_HAS_GID)
                    ? static_cast<gid_t>(info.LxGid)
                    : 0;
}

// Enrich an already-populated struct stat with LxInfo metadata.
// Only overrides st_mode/st_uid/st_gid — all other fields (including st_dev)
// are left untouched from the prior FileStatBasicInformation fill.
LIBC_INLINE void
nt_stat_lx_enrich(const FILE_STAT_LX_INFORMATION &lx, struct stat *out) {
  if (lx.LxFlags & LX_FILE_METADATA_HAS_MODE) {
    mode_t type_bits = out->st_mode & S_IFMT; // Preserve file type from basic.
    out->st_mode = type_bits | (static_cast<mode_t>(lx.LxMode) & 07777);
  }
  if (lx.LxFlags & LX_FILE_METADATA_HAS_UID)
    out->st_uid = static_cast<uid_t>(lx.LxUid);
  if (lx.LxFlags & LX_FILE_METADATA_HAS_GID)
    out->st_gid = static_cast<gid_t>(lx.LxGid);
}

// Fill st_uid/st_gid with the process's euid/egid as a default.
// POSIX: files without explicit ownership metadata are reported as owned
// by the current effective user — this is the fallback when no WSL EAs exist.
LIBC_INLINE void stat_fill_default_uid_gid(struct stat *out) {
  if (out->st_uid == 0)
    out->st_uid = static_cast<uid_t>(
        g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED));
  if (out->st_gid == 0)
    out->st_gid = static_cast<gid_t>(
        g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED));
}

// Enrich st_uid/st_gid from $LXUID/$LXGID EAs on an open handle (for fstat).
// Falls back to euid/egid when EAs are absent.
LIBC_INLINE void stat_uid_gid_from_handle(HANDLE h, struct stat *out) {
  auto uid_result = windows_sec::read_ea_uid(h);
  if (uid_result.has_value())
    out->st_uid = uid_result.value();

  auto gid_result = windows_sec::read_ea_gid(h);
  if (gid_result.has_value())
    out->st_gid = gid_result.value();

  // Fallback: if still 0 (no EAs found), use effective uid/gid.
  stat_fill_default_uid_gid(out);
}

// Three-tier mode reconstruction from an open handle (for fstat).
// Tier 3: $LXMOD EA → exact mode.
// Tier 2: DACL → reverse-mapped owner/group/other bits.
// Tier 1: FILE_ATTRIBUTE_READONLY → 0444 or 0777 (already in base_mode).
//
// POSIX: fstat returns the file's actual mode regardless of how the fd
// was opened. If the handle lacks FILE_READ_EA or READ_CONTROL, we
// reopen the file with the needed access (one syscall).
LIBC_INLINE mode_t stat_mode_from_handle(HANDLE h, mode_t base_mode) {
  // Tier 3: try EA.
  auto ea_result = windows_sec::read_ea_mode(h);
  if (ea_result.has_value()) {
    mode_t type_bits = base_mode & S_IFMT;
    return type_bits | (ea_result.value() & 07777);
  }

  // Tier 2: try DACL. Only recovers the 9 permission bits (rwxrwxrwx).
  // Setuid/setgid/sticky bits have no DACL representation and are lost
  // at this tier — they only survive via Tier 3 (EA).
  auto dacl_result = windows_sec::read_dacl_mode(h);
  if (dacl_result.has_value()) {
    mode_t type_bits = base_mode & S_IFMT;
    return type_bits | (dacl_result.value() & 0777);
  }

  // Both tiers failed. If either returned EACCES, the handle lacks the
  // needed access rights. Reopen with READ_CONTROL + FILE_READ_EA and
  // retry — POSIX requires fstat to return real mode on any valid fd.
  bool access_denied = (ea_result.error() == EACCES) ||
                       (dacl_result.error() == EACCES);
  if (access_denied) {
    HANDLE rh = windows_sec::reopen_with_access(
        h, READ_CONTROL | FILE_READ_EA);
    if (rh) {
      // Retry tier 3 then tier 2 on the reopened handle.
      auto ea2 = windows_sec::read_ea_mode(rh);
      if (ea2.has_value()) {
        ::NtClose(rh);
        mode_t type_bits = base_mode & S_IFMT;
        return type_bits | (ea2.value() & 07777);
      }
      auto dacl2 = windows_sec::read_dacl_mode(rh);
      if (dacl2.has_value()) {
        ::NtClose(rh);
        mode_t type_bits = base_mode & S_IFMT;
        return type_bits | (dacl2.value() & 0777);
      }
      ::NtClose(rh);
    }
  }

  // Tier 1: attribute-based mode already in base_mode.
  return base_mode;
}

// Query volume serial number from a handle (for fstat st_dev).
LIBC_INLINE dev_t query_volume_serial(HANDLE h) {
  alignas(8) UCHAR buf[sizeof(FILE_FS_VOLUME_INFORMATION) + 64];
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryVolumeInformationFile(
      h, &iosb, buf, sizeof(buf), FileFsVolumeInformation);
  if (!NT_SUCCESS(status))
    return 0;
  auto *vol = reinterpret_cast<FILE_FS_VOLUME_INFORMATION *>(buf);
  return static_cast<dev_t>(vol->VolumeSerialNumber);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_NT_TO_STAT_H
