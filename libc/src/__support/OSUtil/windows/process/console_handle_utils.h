//===-- Console handle utilities --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared helpers for console handle validation, safe close, conhost path
// construction, and wide-string buffer manipulation.  Used by console.cpp,
// console_tty.cpp, vt_pty.cpp, pty_tree.cpp, exec_ops.cpp, and spawn_ops.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_HANDLE_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_HANDLE_UTILS_H

#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace console_util {

//===----------------------------------------------------------------------===//
// Console handle sentinel constants
//===----------------------------------------------------------------------===//
//
// PEB->ProcessParameters->ConsoleHandle carries one of these sentinel values
// when the process does not have a live console connection.  The kernel
// interprets them during CreateProcess; they are NOT real handles.

inline constexpr uintptr_t CONSOLE_HANDLE_DETACHED =
    static_cast<uintptr_t>(-1); // CREATE_NEW_CONSOLE detached
inline constexpr uintptr_t CONSOLE_HANDLE_NEW =
    static_cast<uintptr_t>(-2); // CREATE_NEW_CONSOLE pending
inline constexpr uintptr_t CONSOLE_HANDLE_NO_WINDOW =
    static_cast<uintptr_t>(-3); // CREATE_NO_WINDOW

// Returns true when the handle is a real console connection (not null and
// not one of the three sentinel values above).
LIBC_INLINE bool is_live_console_handle(HANDLE handle) {
  uintptr_t bits = reinterpret_cast<uintptr_t>(handle);
  return bits != 0 && bits != CONSOLE_HANDLE_DETACHED &&
         bits != CONSOLE_HANDLE_NEW && bits != CONSOLE_HANDLE_NO_WINDOW;
}

//===----------------------------------------------------------------------===//
// Safe handle close
//===----------------------------------------------------------------------===//

// Closes an NT handle via NtClose if the pointee is non-null, then sets
// the pointee to nullptr.  Safe to call with a null pointer or a null handle.
LIBC_INLINE void close_handle_if_valid(HANDLE *handle) {
  if (handle && *handle) {
    ::NtClose(*handle);
    *handle = nullptr;
  }
}

//===----------------------------------------------------------------------===//
// Wide-string buffer helpers
//===----------------------------------------------------------------------===//

// Append a NUL-terminated wide string to a bounded buffer.
// Returns the new write position, or capacity if the buffer is exhausted.
LIBC_INLINE size_t append_wide(WCHAR *buffer, size_t pos, size_t capacity,
                               const WCHAR *text) {
  for (size_t i = 0; text[i] != 0; ++i) {
    if (pos + 1 >= capacity)
      return capacity; // reserve room for NUL
    buffer[pos++] = text[i];
  }
  return pos;
}

//===----------------------------------------------------------------------===//
// Conhost image path builder
//===----------------------------------------------------------------------===//

// Builds the NT-style image path for conhost.exe:
//   \\??\<NtSystemRoot>\System32\conhost.exe
//
// Reads NtSystemRoot from KUSER_SHARED_DATA.  The caller is responsible for
// appending any command-line arguments after this path.
//
// Returns STATUS_SUCCESS on success, STATUS_BUFFER_TOO_SMALL if capacity
// is insufficient.  On success, *image_path is initialised to point into
// buffer.
LIBC_INLINE NTSTATUS build_conhost_image_path(WCHAR *buffer, size_t capacity,
                                              UNICODE_STRING *image_path) {
  static constexpr WCHAR NT_PATH_PREFIX[] = u"\\??\\";
  static constexpr WCHAR CONHOST_SUFFIX[] = u"\\System32\\conhost.exe";

  const auto *sud = windows_util::shared_user_data();

  size_t pos = 0;
  pos = append_wide(buffer, pos, capacity, NT_PATH_PREFIX);
  pos = append_wide(buffer, pos, capacity, sud->NtSystemRoot);
  pos = append_wide(buffer, pos, capacity, CONHOST_SUFFIX);
  if (pos >= capacity)
    return STATUS_BUFFER_TOO_SMALL;
  buffer[pos] = 0;
  return ::RtlInitUnicodeStringEx(image_path, buffer);
}

//===----------------------------------------------------------------------===//
// Handle duplication helpers
//===----------------------------------------------------------------------===//

// Duplicate a handle without the OBJ_INHERIT attribute.
LIBC_INLINE NTSTATUS duplicate_noninherited(HANDLE source, HANDLE *target) {
  return ::NtDuplicateObject(NtCurrentProcess(), source, NtCurrentProcess(),
                             target, 0, 0, DUPLICATE_SAME_ACCESS);
}

// Duplicate a handle with the OBJ_INHERIT attribute set.
LIBC_INLINE NTSTATUS duplicate_inherited(HANDLE source, HANDLE *target) {
  return ::NtDuplicateObject(NtCurrentProcess(), source, NtCurrentProcess(),
                             target, 0, OBJ_INHERIT, DUPLICATE_SAME_ACCESS);
}

//===----------------------------------------------------------------------===//
// Numeric formatting into WCHAR buffers
//===----------------------------------------------------------------------===//

// Hex-format a uintptr_t with "0x" prefix into a bounded WCHAR buffer.
// Returns the new write position, or capacity if the buffer is exhausted.
LIBC_INLINE size_t append_hex_uintptr(WCHAR *buffer, size_t pos,
                                      size_t capacity, uintptr_t value) {
  static constexpr WCHAR HEX[] = u"0123456789abcdef";
  if (pos + 2 >= capacity)
    return capacity;
  buffer[pos++] = u'0';
  buffer[pos++] = u'x';

  bool started = false;
  for (int shift = static_cast<int>(sizeof(uintptr_t) * 8) - 4; shift >= 0;
       shift -= 4) {
    unsigned digit = static_cast<unsigned>((value >> shift) & 0xF);
    if (!started && digit == 0 && shift != 0)
      continue;
    started = true;
    if (pos + 1 >= capacity)
      return capacity;
    buffer[pos++] = HEX[digit];
  }
  return pos;
}

// Decimal-format a uint16_t into a bounded WCHAR buffer.
// Returns the new write position, or capacity if the buffer is exhausted.
LIBC_INLINE size_t append_uint16(WCHAR *buffer, size_t pos, size_t capacity,
                                 uint16_t value) {
  uint16_t divisor = 10000;
  bool started = false;
  while (divisor != 0) {
    uint16_t digit = static_cast<uint16_t>(value / divisor);
    if (digit != 0 || started || divisor == 1) {
      started = true;
      if (pos + 1 >= capacity)
        return capacity;
      buffer[pos++] = static_cast<WCHAR>(u'0' + digit);
    }
    value = static_cast<uint16_t>(value % divisor);
    divisor = static_cast<uint16_t>(divisor / 10);
  }
  return pos;
}

//===----------------------------------------------------------------------===//
// Screen buffer window-size extraction
//===----------------------------------------------------------------------===//

/// Extract row/col counts from a ConDrv CONSOLE_SMALL_RECT window rectangle.
/// Signed arithmetic detects inverted rects (result <= 0 → 0). The cast to
/// uint16_t is always safe after the > 0 check because LONG max (2^31-1)
/// can't actually exceed 0xFFFF for console dimensions — the clamp is
/// defensive belt-and-suspenders only.
LIBC_INLINE void srwindow_to_rowcol(const condrv::CONSOLE_SMALL_RECT &sr,
                                    uint16_t *rows, uint16_t *cols) {
  LONG r = static_cast<LONG>(sr.Bottom) - static_cast<LONG>(sr.Top) + 1;
  LONG c = static_cast<LONG>(sr.Right) - static_cast<LONG>(sr.Left) + 1;
  *rows = r > 0 ? static_cast<uint16_t>(r) : static_cast<uint16_t>(0);
  *cols = c > 0 ? static_cast<uint16_t>(c) : static_cast<uint16_t>(0);
}

} // namespace console_util
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_HANDLE_UTILS_H
