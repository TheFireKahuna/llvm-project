//===-- NT-native file type classification -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared handle classification helper used by the fd table and NT-native tests.
// This reproduces Win32 GetFileType semantics from NT volume metadata rather
// than going through kernel32.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FILE_TYPE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FILE_TYPE_H

#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/macros/attributes.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

LIBC_INLINE DWORD query_file_type(HANDLE handle) {
  if (!handle || handle == INVALID_HANDLE_VALUE)
    return FILE_TYPE_UNKNOWN;

  FILE_FS_DEVICE_INFORMATION device_info = {};
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = NtQueryVolumeInformationFile(
      handle, &iosb, &device_info, sizeof(device_info),
      FileFsDeviceInformation);
  if (!NT_SUCCESS(status))
    return FILE_TYPE_UNKNOWN;

  switch (device_info.DeviceType) {
  case FILE_DEVICE_CD_ROM:
  case FILE_DEVICE_CD_ROM_FILE_SYSTEM:
  case FILE_DEVICE_CONTROLLER:
  case FILE_DEVICE_DATALINK:
  case FILE_DEVICE_DFS:
  case FILE_DEVICE_DISK:
  case FILE_DEVICE_DISK_FILE_SYSTEM:
  case FILE_DEVICE_VIRTUAL_DISK:
    return FILE_TYPE_DISK;

  case FILE_DEVICE_KEYBOARD:
  case FILE_DEVICE_MIDI_OUT:
  case FILE_DEVICE_NETWORK_FILE_SYSTEM:
  case FILE_DEVICE_NULL:
  case FILE_DEVICE_PHYSICAL_NETCARD:
  case FILE_DEVICE_SERIAL_MOUSE_PORT:
  case FILE_DEVICE_SERIAL_PORT:
  case FILE_DEVICE_SCREEN:
  case FILE_DEVICE_MODEM:
  case FILE_DEVICE_CONSOLE:
    return FILE_TYPE_CHAR;

  case FILE_DEVICE_NAMED_PIPE:
    return FILE_TYPE_PIPE;

  default:
    return FILE_TYPE_UNKNOWN;
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FILE_TYPE_H
