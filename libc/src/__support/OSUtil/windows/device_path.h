//===-- Device path to DOS path resolution -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Resolves an NT device path (e.g., \Device\HarddiskVolume3\Users\foo) to a
// DOS path (e.g., C:\Users\foo) by querying the mount manager. Handles drive
// letters, volume GUIDs, and mounted folders.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEVICE_PATH_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEVICE_PATH_H

#include "hdr/types/size_t.h"
#include "src/__support/CPP/span.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_util {

// Resolves an NT object path from NtQueryObject(ObjectNameInformation) to a
// full DOS path. Writes the result as a wide (UTF-16) string into dos_buf.
// Returns the length in WCHARs (excluding NUL), or 0 on failure.
//
// Example:
//   nt_path  = L"\Device\HarddiskVolume3\Users\foo\project"
//   dos_buf  = L"C:\Users\foo\project"
//   returns 24
LIBC_INLINE size_t device_path_to_dos(const WCHAR *nt_path,
                                       ULONG nt_path_chars, WCHAR *dos_buf,
                                       size_t dos_buf_wchars) {
  // Find the end of the device name prefix. The path from NtQueryObject looks
  // like "\Device\HarddiskVolume3\Users\foo". We need to extract
  // "\Device\HarddiskVolume3" as the device name and "\Users\foo" as the
  // volume-relative tail.

  // Skip the first backslash to start searching for the third one.
  // Pattern: \Device\HarddiskVolumeN\... — we want the 3rd backslash.
  ULONG slash_count = 0;
  ULONG device_prefix_len = nt_path_chars; // default: entire path is device
  for (ULONG i = 0; i < nt_path_chars; ++i) {
    if (nt_path[i] == u'\\') {
      ++slash_count;
      if (slash_count == 3) {
        device_prefix_len = i;
        break;
      }
    }
  }

  // Open the mount manager.
  WCHAR mm_path[] = u"\\Device\\MountPointManager";
  windows::nt_wstring_view mm_name(mm_path);

  auto oa = windows::named_internal_oa(&mm_name);

  IO_STATUS_BLOCK iosb = {};
  windows::ScopedNtHandle mm_handle;
  NTSTATUS status =
      ::NtOpenFile(mm_handle.put(), SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa, &iosb,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return 0;

  // Build MOUNTMGR_TARGET_NAME with the device prefix.
  // Layout: USHORT DeviceNameLength + WCHAR DeviceName[device_prefix_len]
  // Device prefixes are short (e.g. \Device\HarddiskVolume3, ~25 chars),
  // but use a generous buffer to avoid artificial limits.
  constexpr size_t MAX_DEVICE_PREFIX = 512;
  alignas(MOUNTMGR_TARGET_NAME) char
      input_buf[sizeof(USHORT) + MAX_DEVICE_PREFIX * sizeof(WCHAR)];
  auto *target = reinterpret_cast<MOUNTMGR_TARGET_NAME *>(input_buf);
  ULONG device_bytes = device_prefix_len * sizeof(WCHAR);

  if (device_prefix_len > MAX_DEVICE_PREFIX)
    return 0;

  target->DeviceNameLength = static_cast<USHORT>(device_bytes);
  __builtin_memcpy(target->DeviceName, nt_path,
                   device_prefix_len * sizeof(WCHAR));

  ULONG input_size =
      static_cast<ULONG>(sizeof(USHORT) + device_bytes);

  // Query the DOS volume path. The result is a volume root (e.g. "C:\")
  // which is always short, but use a generous buffer for robustness.
  constexpr size_t MAX_VOLUME_ROOT = 512;
  alignas(MOUNTMGR_VOLUME_PATHS) char
      output_buf[sizeof(ULONG) + MAX_VOLUME_ROOT * sizeof(WCHAR)];
  auto *paths = reinterpret_cast<MOUNTMGR_VOLUME_PATHS *>(output_buf);

  status = ::NtDeviceIoControlFile(
      mm_handle.get(), nullptr, nullptr, nullptr, &iosb,
      IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH, target, input_size, paths,
      static_cast<ULONG>(sizeof(output_buf)));

  if (!NT_SUCCESS(status) || paths->MultiSzLength == 0)
    return 0;

  // paths->MultiSz contains the DOS volume root, e.g. "C:\".
  // Strip the trailing backslash from the mount point for concatenation.
  ULONG mount_chars = paths->MultiSzLength / sizeof(WCHAR);
  // Remove trailing NULs from multi-sz.
  while (mount_chars > 0 && paths->MultiSz[mount_chars - 1] == u'\0')
    --mount_chars;
  // Remove trailing backslash (e.g., "C:\" → "C:").
  if (mount_chars > 0 && paths->MultiSz[mount_chars - 1] == u'\\')
    --mount_chars;

  // The volume-relative tail starts at device_prefix_len in nt_path.
  // It looks like "\Users\foo\project" (starts with backslash).
  const WCHAR *tail = nt_path + device_prefix_len;
  ULONG tail_chars = nt_path_chars - device_prefix_len;

  ULONG total = mount_chars + tail_chars;
  if (total + 1 > dos_buf_wchars)
    return 0;

  // Assemble: "C:" + "\Users\foo\project"
  windows::WStringStream ss(cpp::span<WCHAR>(dos_buf, dos_buf_wchars - 1));
  ss << windows::nt_wstring_view(paths->MultiSz, mount_chars)
     << windows::nt_wstring_view(tail, tail_chars);
  ss.null_terminate();

  return ss.str().size();
}

} // namespace windows_util
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEVICE_PATH_H
