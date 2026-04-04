//===-- Console byte-stream I/O helpers -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Direct byte-stream I/O for ConDrv handles.
//
// The control plane (mode, lifecycle, input events, screen-buffer state) uses
// explicit ConDrv control messages in ipc/condrv.h. The data plane for console
// handles, however, follows the normal file-style path used by Terminal's own
// API tests: ReadFile/WriteFile semantics on the input/output handles.
//
// These helpers intentionally bypass IoRing for console handles.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_CONSOLE_FILE_IO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_CONSOLE_FILE_IO_H

#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/OSUtil/windows/memory/mem_fault_handler.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace console_file_io {

LIBC_INLINE ULONG clamp_io_len(size_t count) {
  return count > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<ULONG>(count);
}

/// Classify a console I/O NTSTATUS into a ssize_t error or sentinel.
/// Returns:
///   -EINTR  for STATUS_CANCELLED (interrupted by signal)
///   0       for STATUS_END_OF_FILE / STATUS_PIPE_BROKEN (EOF/hangup on read)
///   negative errno for all other failures
/// Also delivers SIGHUP for terminal-death statuses.
LIBC_INLINE ssize_t classify_console_status(NTSTATUS status,
                                            bool is_read) {
  if (status == STATUS_INVALID_HANDLE || status == STATUS_PIPE_BROKEN)
    console_tty::deliver_terminal_hangup();
  if (status == STATUS_CANCELLED)
    return -EINTR;
  if (is_read &&
      (status == STATUS_END_OF_FILE || status == STATUS_PIPE_BROKEN))
    return 0;
  return -windows_util::ntstatus_to_errno(status);
}

LIBC_INLINE ssize_t read(HANDLE handle, void *buffer, size_t count) {
  if (!handle)
    return -EBADF;
  if (count == 0)
    return 0;

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtReadFile(handle, nullptr, nullptr, nullptr, &iosb,
                                 buffer, clamp_io_len(count), nullptr, nullptr);
  if (!NT_SUCCESS(status))
    return classify_console_status(status, true);
  if (!NT_SUCCESS(iosb.Status))
    return classify_console_status(iosb.Status, true);

  return static_cast<ssize_t>(iosb.Information);
}

LIBC_INLINE ssize_t write(HANDLE handle, const void *buffer, size_t count) {
  if (!handle)
    return -EBADF;
  if (count == 0)
    return 0;

  ULONG io_len = clamp_io_len(count);
  windows::prefault_read_pages(buffer, io_len);

  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status =
      ::NtWriteFile(handle, nullptr, nullptr, nullptr, &iosb,
                    const_cast<void *>(buffer), io_len, nullptr, nullptr);
  if (!NT_SUCCESS(status))
    return classify_console_status(status, false);
  if (!NT_SUCCESS(iosb.Status))
    return classify_console_status(iosb.Status, false);

  console_tty::observe_output_resize(handle);
  return static_cast<ssize_t>(iosb.Information);
}

} // namespace console_file_io
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_CONSOLE_FILE_IO_H
