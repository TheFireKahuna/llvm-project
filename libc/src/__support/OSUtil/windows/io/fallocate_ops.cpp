//===-- Windows internal fallocate operation --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Preallocates disk space via NtSetInformationFile(FileAllocationInformation).
// The filesystem reserves contiguous clusters up to AllocationSize without
// changing the visible file size (logical EOF).
//
// Returns 0 on success, positive errno on failure (posix_fallocate convention).
//
//===----------------------------------------------------------------------===//

#include "fallocate_ops.h"
#include "hdr/errno_macros.h"
#include "hdr/types/off_t.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/syscall.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

int posix_fallocate(int fd, off_t offset, off_t len) {
  if (offset < 0 || len <= 0)
    return EINVAL;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return EBADF;
  if (ofd->is_path_only())
    return EBADF;

  HANDLE h = ofd->handle;

  // FileAllocationInformation preallocates disk space up to AllocationSize
  // without changing the logical EOF. If AllocationSize is smaller than the
  // current allocation, this is a no-op (not an error).
  FILE_ALLOCATION_INFORMATION info;
  info.AllocationSize.QuadPart =
      static_cast<LONGLONG>(offset) + static_cast<LONGLONG>(len);

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtSetInformationFile(
      h, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      FileAllocationInformation);

  if (NT_SUCCESS(status))
    return 0;

  if (status == STATUS_ACCESS_DENIED)
    return EBADF; // fd not open for writing
  if (status == STATUS_DISK_FULL)
    return ENOSPC;
  return windows_util::ntstatus_to_errno(status);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
