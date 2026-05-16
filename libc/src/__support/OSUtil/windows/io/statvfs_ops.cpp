//===-- Internal statvfs engine implementation -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Business logic for statvfs, fstatvfs, and the shared statvfs_from_handle
// helper. Returns 0 on success, -errno on failure. No libc_errno references.
//
//===----------------------------------------------------------------------===//

#include "statvfs_ops.h"

#include "hdr/errno_macros.h"
#include "include/llvm-libc-types/struct_statvfs.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/syscall.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

// POSIX mount flags (not yet in libc headers).
#ifndef ST_RDONLY
#define ST_RDONLY 1
#endif
#ifndef ST_NOSUID
#define ST_NOSUID 2
#endif

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ---------------------------------------------------------------------------
// statvfs_from_handle engine — shared by statvfs() and fstatvfs()
// ---------------------------------------------------------------------------
intptr_t statvfs_from_handle(void *handle, struct statvfs *buf) {
  HANDLE h = static_cast<HANDLE>(handle);
  IO_STATUS_BLOCK iosb;

  // Query quota-aware block counts.
  FILE_FS_FULL_SIZE_INFORMATION fsi;
  NTSTATUS status = NtQueryVolumeInformationFile(
      h, &iosb, &fsi, sizeof(fsi), FileFsFullSizeInformation);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  ULONG block_size = fsi.SectorsPerAllocationUnit * fsi.BytesPerSector;

  buf->f_bsize = block_size;
  buf->f_frsize = block_size;
  buf->f_blocks = static_cast<fsblkcnt_t>(fsi.TotalAllocationUnits.QuadPart);
  buf->f_bfree =
      static_cast<fsblkcnt_t>(fsi.ActualAvailableAllocationUnits.QuadPart);
  buf->f_bavail =
      static_cast<fsblkcnt_t>(fsi.CallerAvailableAllocationUnits.QuadPart);

  // Query NTFS MFT record count for inode statistics. Falls back to 0
  // on non-NTFS filesystems (ReFS, FAT, etc.) where this FSCTL fails.
  buf->f_files = 0;
  buf->f_ffree = 0;
  buf->f_favail = 0;
  NTFS_VOLUME_DATA_BUFFER nvdb;
  NTSTATUS nvdb_status = NtFsControlFile(
      h, nullptr, nullptr, nullptr, &iosb, FSCTL_GET_NTFS_VOLUME_DATA,
      nullptr, 0, &nvdb, sizeof(nvdb));
  if (NT_SUCCESS(nvdb_status) && nvdb.BytesPerFileRecordSegment > 0) {
    fsfilcnt_t existing_records = static_cast<fsfilcnt_t>(
        nvdb.MftValidDataLength.QuadPart / nvdb.BytesPerFileRecordSegment);
    // NTFS auto-extends the MFT from free clusters, so free inodes are
    // effectively bounded by free disk space.
    fsfilcnt_t free_from_space = static_cast<fsfilcnt_t>(
        nvdb.FreeClusters.QuadPart * nvdb.BytesPerCluster /
        nvdb.BytesPerFileRecordSegment);
    // f_files = total capacity (existing + potential from free space),
    // ensuring f_ffree <= f_files always holds.
    buf->f_files = existing_records + free_from_space;
    buf->f_ffree = free_from_space;
    buf->f_favail = free_from_space;
  }

  // Query filesystem attributes for f_namemax and f_flag.
  alignas(8) char attr_buf[128];
  status = NtQueryVolumeInformationFile(h, &iosb, attr_buf, sizeof(attr_buf),
                                        FileFsAttributeInformation);
  if (NT_SUCCESS(status)) {
    auto *attr = reinterpret_cast<FILE_FS_ATTRIBUTE_INFORMATION *>(attr_buf);
    buf->f_namemax = attr->MaximumComponentNameLength;
    buf->f_flag = ST_NOSUID; // Windows has no setuid/setgid concept.
    if (attr->FileSystemAttributes & FILE_READ_ONLY_VOLUME)
      buf->f_flag |= ST_RDONLY;
  } else {
    buf->f_namemax = 255;
    buf->f_flag = ST_NOSUID;
  }

  // Query volume serial number for f_fsid.
  alignas(8) char vol_buf[sizeof(FILE_FS_VOLUME_INFORMATION) +
                           256 * sizeof(WCHAR)];
  status = NtQueryVolumeInformationFile(h, &iosb, vol_buf, sizeof(vol_buf),
                                        FileFsVolumeInformation);
  if (NT_SUCCESS(status)) {
    auto *vol = reinterpret_cast<FILE_FS_VOLUME_INFORMATION *>(vol_buf);
    buf->f_fsid = vol->VolumeSerialNumber;
  } else {
    buf->f_fsid = 0;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// statvfs engine
// ---------------------------------------------------------------------------
intptr_t statvfs(const char *__restrict path, struct statvfs *__restrict buf) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return -EFAULT;
  string_view sv(path);

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  auto nt = to_nt_path(sv, path_buf, path_buf_s.size());
  if (!nt.has_value())
    return -nt.error();
  size_t nt_len = nt.value();

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name(path_buf, nt_len);
  init_object_attributes(&oa, &name);

  IO_STATUS_BLOCK iosb;
  HANDLE handle;

  NTSTATUS status = NtOpenFile(
      &handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  intptr_t result = statvfs_from_handle(handle, buf);
  NtClose(handle);
  return result;
}

// ---------------------------------------------------------------------------
// fstatvfs engine
// ---------------------------------------------------------------------------
intptr_t fstatvfs(int fd, struct statvfs *buf) {
  auto handle = internal::fd_table.get(fd);
  if (!handle)
    return -handle.error();

  return statvfs_from_handle(static_cast<void *>(*handle), buf);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
