//===-- POSIX termios default values for console/PTY terminals --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared termios initialisation helpers used by both the ConDrv console
// (console_tty.cpp) and VT PTY (vt_pty.cpp) code paths.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMIOS_DEFAULTS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMIOS_DEFAULTS_H

#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace termios_defaults {

//===----------------------------------------------------------------------===//
// Default baud rate
//===----------------------------------------------------------------------===//

inline constexpr speed_t DEFAULT_TERMINAL_SPEED = B38400;

//===----------------------------------------------------------------------===//
// Named POSIX control character defaults (POSIX Table 11-1)
//===----------------------------------------------------------------------===//

inline constexpr cc_t CC_VINTR  = 3;   // Ctrl-C
inline constexpr cc_t CC_VQUIT  = 28;  // Ctrl-backslash
inline constexpr cc_t CC_VERASE = 8;   // Ctrl-H (backspace)
inline constexpr cc_t CC_VKILL  = 21;  // Ctrl-U
inline constexpr cc_t CC_VEOF   = 4;   // Ctrl-D
inline constexpr cc_t CC_VTIME  = 0;
inline constexpr cc_t CC_VMIN   = 1;
inline constexpr cc_t CC_VSTART = 17;  // Ctrl-Q
inline constexpr cc_t CC_VSTOP  = 19;  // Ctrl-S
inline constexpr cc_t CC_VSUSP  = 26;  // Ctrl-Z

// Populate the c_cc[] array with sane POSIX defaults.
LIBC_INLINE void set_default_control_chars(struct termios *attrs) {
  for (size_t i = 0; i < NCCS; ++i)
    attrs->c_cc[i] = _POSIX_VDISABLE;

  attrs->c_cc[VINTR]  = CC_VINTR;
  attrs->c_cc[VQUIT]  = CC_VQUIT;
  attrs->c_cc[VERASE] = CC_VERASE;
  attrs->c_cc[VKILL]  = CC_VKILL;
  attrs->c_cc[VEOF]   = CC_VEOF;
  attrs->c_cc[VTIME]  = CC_VTIME;
  attrs->c_cc[VMIN]   = CC_VMIN;
  attrs->c_cc[VSTART] = CC_VSTART;
  attrs->c_cc[VSTOP]  = CC_VSTOP;
  attrs->c_cc[VSUSP]  = CC_VSUSP;
}

//===----------------------------------------------------------------------===//
// ConDrv mode requests
//===----------------------------------------------------------------------===//
//
// The libc tty discipline owns canonical editing, echo, and signal generation
// in user space.  The host input queue stays in raw event mode; only
// window-size notifications pass through (for SIGWINCH generation).

LIBC_INLINE DWORD desired_input_mode(const struct termios &) {
  return condrv::ENABLE_WINDOW_INPUT;
}

// A POSIX tty always treats CR/LF/VT as terminal control, even with OPOST
// disabled.  Keep the host terminal parser enabled; termios-directed byte
// transformations are applied in user space.
LIBC_INLINE DWORD desired_output_mode(const struct termios &) {
  return condrv::ENABLE_PROCESSED_OUTPUT |
         condrv::ENABLE_VIRTUAL_TERMINAL_PROCESSING;
}

//===----------------------------------------------------------------------===//
// Full termios initialisation (equivalent to `stty sane`)
//===----------------------------------------------------------------------===//

LIBC_INLINE void init_default_termios(struct termios *attrs) {
  *attrs = {};
  attrs->c_iflag = BRKINT | ICRNL | IXON;
  attrs->c_oflag = OPOST | ONLCR;
  attrs->c_cflag = CREAD | CS8 | CLOCAL | DEFAULT_TERMINAL_SPEED;
  attrs->c_lflag = ICANON | ISIG | ECHO | ECHOE | ECHOK;
  set_default_control_chars(attrs);
}

} // namespace termios_defaults
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_TERMIOS_DEFAULTS_H
