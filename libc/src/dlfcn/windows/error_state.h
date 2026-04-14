//===-- Thread-local dlfcn error state for Windows --------------- C++ --*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX dlerror() requires per-thread error state that clears on read.
// Pure ntdll — no kernel32 dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_DLFCN_WINDOWS_ERROR_STATE_H
#define LLVM_LIBC_SRC_DLFCN_WINDOWS_ERROR_STATE_H

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace dlfcn_state {

inline constexpr int ERROR_BUF_SIZE = 128;

struct ThreadState {
  char message[ERROR_BUF_SIZE];
  bool has_error;
};

LIBC_INLINE ThreadState &get_state() {
  static thread_local ThreadState state = {{}, false};
  return state;
}

// Loader NTSTATUS → terse dlerror message.
LIBC_INLINE const char *ntstatus_to_dl_message(NTSTATUS status) {
  switch (status) {
  case STATUS_DLL_NOT_FOUND:
  case STATUS_OBJECT_NAME_NOT_FOUND:
  case STATUS_OBJECT_PATH_NOT_FOUND:
    return "module not found";
  case STATUS_ENTRYPOINT_NOT_FOUND:
    return "symbol not found";
  case STATUS_ACCESS_DENIED:
    return "access denied";
  case STATUS_NO_MEMORY:
  case STATUS_INSUFFICIENT_RESOURCES:
    return "out of memory";
  case STATUS_INVALID_IMAGE_FORMAT:
    return "invalid image format";
  case STATUS_INVALID_PARAMETER:
    return "invalid parameter";
  default:
    return nullptr; // Use hex fallback.
  }
}

// Copy a string into the thread-local buffer.
LIBC_INLINE void copy_message(ThreadState &s, const char *msg) {
  int i = 0;
  for (; msg[i] && i < ERROR_BUF_SIZE - 1; ++i)
    s.message[i] = msg[i];
  s.message[i] = '\0';
}

// Format NTSTATUS as hex into the buffer ("NTSTATUS 0xNNNNNNNN").
LIBC_INLINE void format_hex_status(ThreadState &s, NTSTATUS status) {
  const char prefix[] = "NTSTATUS 0x";
  int i = 0;
  for (; prefix[i]; ++i)
    s.message[i] = prefix[i];
  auto val = static_cast<unsigned long>(status);
  for (int shift = 28; shift >= 0; shift -= 4) {
    unsigned nibble = (val >> shift) & 0xF;
    s.message[i++] =
        static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
  }
  s.message[i] = '\0';
}

// Record a dlfcn failure from an NTSTATUS code.
LIBC_INLINE void set_error(NTSTATUS status) {
  ThreadState &s = get_state();
  s.has_error = true;
  const char *msg = ntstatus_to_dl_message(status);
  if (msg)
    copy_message(s, msg);
  else
    format_hex_status(s, status);
}

// Record a dlfcn failure with a custom message.
LIBC_INLINE void set_error_message(const char *msg) {
  ThreadState &s = get_state();
  s.has_error = true;
  copy_message(s, msg);
}

LIBC_INLINE void clear_error() { get_state().has_error = false; }

// Map an errno value to a descriptive dlfcn error message.
// Used by entry points to convert ErrorOr failures to dlerror-visible messages.
LIBC_INLINE void set_error_from_errno(int err) {
  switch (err) {
  case ENOENT:
    set_error_message("module or symbol not found");
    break;
  case EACCES:
    set_error_message("access denied");
    break;
  case ENOMEM:
    set_error_message("out of memory");
    break;
  case ENOEXEC:
    set_error_message("invalid image format");
    break;
  case EINVAL:
    set_error_message("invalid parameter");
    break;
  case ENOSYS:
    set_error_message("not supported");
    break;
  case ESRCH:
    set_error_message("handle not found");
    break;
  case EIO:
  default:
    set_error_message("unknown error");
    break;
  }
}

// Consume the error message. Returns nullptr if no error pending.
LIBC_INLINE char *consume_error() {
  ThreadState &s = get_state();
  if (!s.has_error)
    return nullptr;
  s.has_error = false;
  return s.message;
}

} // namespace dlfcn_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_DLFCN_WINDOWS_ERROR_STATE_H
