//===-- Internal utimes/lutimes/futimes engine implementation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Business logic for utimes, lutimes, and futimes.
// Returns 0 on success, -errno on failure. No libc_errno references.
//
//===----------------------------------------------------------------------===//

#include "utimes_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/sys_stat_macros.h"
#include "hdr/types/struct_timespec.h"
#include "hdr/types/struct_timeval.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Shared constants
// =========================================================================

// 100ns ticks per second / per microsecond.
static constexpr LONGLONG HNS_PER_SEC = 10'000'000LL;
static constexpr LONGLONG HNS_PER_USEC = 10LL;

// 100ns ticks between Windows epoch (1601-01-01) and Unix epoch (1970-01-01).
static constexpr LONGLONG NT_EPOCH_DELTA = 116'444'736'000'000'000LL;

// =========================================================================
// Anonymous-namespace helpers
// =========================================================================

namespace {

/// Validate tv_usec fields of a timeval pair. Returns 0 if valid, -EINVAL if
/// either tv_usec is out of the [0, 999999] range.
intptr_t validate_times(const struct timeval times[2]) {
  if (times) {
    if (times[0].tv_usec < 0 || times[0].tv_usec >= 1000000 ||
        times[1].tv_usec < 0 || times[1].tv_usec >= 1000000)
      return -EINVAL;
  }
  return 0;
}

intptr_t validate_timespecs(const struct timespec times[2]) {
  if (!times)
    return 0;

  for (int i = 0; i < 2; ++i) {
    long tv_nsec = times[i].tv_nsec;
    if (tv_nsec == UTIME_NOW || tv_nsec == UTIME_OMIT)
      continue;
    if (tv_nsec < 0 || tv_nsec >= 1000000000L)
      return -EINVAL;
  }
  return 0;
}

/// Build FILE_BASIC_INFORMATION from a timeval pair (or current time if NULL)
/// and apply it to the given handle via NtSetInformationFile.
/// Returns 0 on success, -errno on failure.
intptr_t set_file_times(HANDLE h, const struct timeval times[2]) {
  FILE_BASIC_INFORMATION basic = {};
  if (times) {
    basic.LastAccessTime.QuadPart =
        static_cast<LONGLONG>(times[0].tv_sec) * HNS_PER_SEC +
        static_cast<LONGLONG>(times[0].tv_usec) * HNS_PER_USEC +
        NT_EPOCH_DELTA;
    basic.LastWriteTime.QuadPart =
        static_cast<LONGLONG>(times[1].tv_sec) * HNS_PER_SEC +
        static_cast<LONGLONG>(times[1].tv_usec) * HNS_PER_USEC +
        NT_EPOCH_DELTA;
  } else {
    // NULL times -> set both to current time (already in Windows epoch).
    LONGLONG now = ::RtlGetSystemTimePrecise();
    basic.LastAccessTime.QuadPart = now;
    basic.LastWriteTime.QuadPart = now;
  }

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtSetInformationFile(h, &iosb, &basic, sizeof(basic),
                                           FileBasicInformation);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  return 0;
}

intptr_t query_file_times(HANDLE h, FILE_BASIC_INFORMATION &basic) {
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryInformationFile(
      h, &iosb, &basic, sizeof(basic), FileBasicInformation);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  return 0;
}

intptr_t set_file_times(HANDLE h, const struct timespec times[2]) {
  FILE_BASIC_INFORMATION basic = {};
  intptr_t ret = query_file_times(h, basic);
  if (ret)
    return ret;

  LONGLONG now = 0;
  auto get_now = [&]() -> LONGLONG {
    if (now == 0)
      now = ::RtlGetSystemTimePrecise();
    return now;
  };

  auto to_filetime = [](const struct timespec &ts) -> LONGLONG {
    return static_cast<LONGLONG>(ts.tv_sec) * HNS_PER_SEC +
           static_cast<LONGLONG>(ts.tv_nsec) / 100 +
           NT_EPOCH_DELTA;
  };

  if (!times) {
    LONGLONG current = get_now();
    basic.LastAccessTime.QuadPart = current;
    basic.LastWriteTime.QuadPart = current;
  } else {
    if (times[0].tv_nsec == UTIME_NOW)
      basic.LastAccessTime.QuadPart = get_now();
    else if (times[0].tv_nsec != UTIME_OMIT)
      basic.LastAccessTime.QuadPart = to_filetime(times[0]);

    if (times[1].tv_nsec == UTIME_NOW)
      basic.LastWriteTime.QuadPart = get_now();
    else if (times[1].tv_nsec != UTIME_OMIT)
      basic.LastWriteTime.QuadPart = to_filetime(times[1]);
  }

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtSetInformationFile(h, &iosb, &basic, sizeof(basic),
                                           FileBasicInformation);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  return 0;
}

/// Open a file by path and set its timestamps. The open_flags parameter
/// controls symlink behavior: 0 follows symlinks, FILE_OPEN_REPARSE_POINT
/// operates on the symlink itself.
intptr_t utimes_by_path(const char *path, const struct timeval times[2],
                    ULONG open_flags) {
  intptr_t err = validate_times(times);
  if (err)
    return err;

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  size_t len = to_nt_path(path, path_buf, path_buf_s.size());
  if (len == 0)
    return -EINVAL;

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, path_buf, len);

  HANDLE handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      &handle, FILE_WRITE_ATTRIBUTES, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, open_flags);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  intptr_t ret = set_file_times(handle, times);
  ::NtClose(handle);
  return ret;
}

} // anonymous namespace

// =========================================================================
// internal::utimes
// =========================================================================

intptr_t utimes(const char *path, const struct timeval times[2]) {
  return utimes_by_path(path, times, /*open_flags=*/0);
}

// =========================================================================
// internal::lutimes
// =========================================================================

intptr_t lutimes(const char *path, const struct timeval times[2]) {
  return utimes_by_path(path, times, FILE_OPEN_REPARSE_POINT);
}

// =========================================================================
// internal::futimes
// =========================================================================

intptr_t futimes(int fd, const struct timeval times[2]) {
  intptr_t err = validate_times(times);
  if (err)
    return err;

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (ofd->is_path_only())
    return -EBADF;

  return set_file_times(ofd->handle, times);
}

intptr_t utimensat(int dirfd, const char *path,
                   const struct timespec times[2], int flags) {
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return -EINVAL;

  intptr_t err = validate_timespecs(times);
  if (err)
    return err;

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  int resolve_err =
      resolve_at_path(dirfd, path, path_buf, path_buf_s.size(), &oa, &us,
                      flags);
  if (resolve_err)
    return -resolve_err;

  ULONG open_opts = FILE_OPEN_FOR_BACKUP_INTENT;
  if (flags & AT_SYMLINK_NOFOLLOW)
    open_opts |= FILE_OPEN_REPARSE_POINT;

  HANDLE handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtOpenFile(
      &handle, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, open_opts);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  intptr_t ret = set_file_times(handle, times);
  ::NtClose(handle);
  return ret;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
