//===-- Process-resident tty state for Windows console ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_STATE_H

#include "hdr/types/pid_t.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "hdr/unistd_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Snapshot of terminal state fields that are shared between processes (for
/// PTY-attached mode) or stored locally (for standalone console). Populated
/// by snapshot_terminal_state_locked() under ConsoleTtyState::lock, providing
/// a consistent view of attrs/sid/pgid for the duration of one I/O operation.
struct TerminalStateSnapshot {
  struct termios attrs;
  pid_t controlling_sid;
  pid_t foreground_pgid;
  bool has_terminal;
};

// Canonical buffer capacity: _POSIX_MAX_INPUT (255 bytes).
//
// Tradeoff: 255 bytes is the POSIX minimum guaranteed canonical line length.
// Linux uses 4096 (N_TTY_BUF_SIZE) which handles long paths and shell lines
// better, but costs 16x more memory per-process. We choose the POSIX minimum
// because (a) the ConsoleTtyState struct lives in the process control block and
// is never heap-allocated — keeping it compact matters, (b) programs that need
// longer lines should use non-canonical mode, and (c) the ready queue below
// doubles this budget for pipelining. If real-world breakage occurs (lines
// silently truncated at 255 bytes in canonical mode), increase this to 4096
// and update the static_assert below.
inline constexpr size_t CONSOLE_TTY_CANONICAL_CAPACITY = _POSIX_MAX_INPUT;
// The ready queue must absorb one full unread canonical line while the next
// full line is committed, so budget two _POSIX_MAX_INPUT-sized lines.
inline constexpr size_t CONSOLE_TTY_READY_LINE_BUDGET = 2;
inline constexpr size_t CONSOLE_TTY_READY_CAPACITY =
    CONSOLE_TTY_CANONICAL_CAPACITY * CONSOLE_TTY_READY_LINE_BUDGET;

/// Process-wide terminal state for the ConDrv-backed console TTY.
///
/// Fields use uint8_t/uint16_t rather than bool/size_t for compact packing:
/// the struct lives in the process control block and is never heap-allocated,
/// so keeping it under one cache line for the hot fields matters. The bool
/// equivalent fields (has_terminal, output_stopped) are 0/1 only.
struct ConsoleTtyState {
  RawMutex lock;

  /// Non-zero after first use. Always accessed under lock — the atomic is
  /// intentionally belt-and-suspenders: it provides an acquire/release barrier
  /// that guarantees initialization stores are visible to threads that
  /// subsequently observe initialized==1, independent of the lock's memory
  /// ordering guarantees. This ensures correctness even if the lock
  /// implementation changes (e.g., to a try-lock fast path that doesn't
  /// issue a full fence). Do not remove the atomic or replace with plain
  /// uint32_t without auditing all callers.
  cpp::Atomic<uint32_t> initialized;

  /// Current effective termios state. Modified by tcsetattr, synced from PTY
  /// tree when attached.
  struct termios attrs;

  /// Session ID of the controlling terminal owner. 0 = no controlling terminal.
  pid_t controlling_sid;

  /// Foreground process group. Reads/writes to the terminal by background
  /// groups generate SIGTTIN/SIGTTOU if TOSTOP is set.
  pid_t foreground_pgid;

  /// 1 if this process has a controlling terminal, 0 otherwise.
  uint8_t has_terminal;

  /// 1 if output is suspended via XOFF (^S), cleared on XON (^Q).
  uint8_t output_stopped;

  /// Futex for XON/XOFF flow control wait. Value mirrors output_stopped
  /// (0 = flowing, 1 = stopped). Writers use store_and_notify_all on XON
  /// to wake blocked output threads instead of relying on polling.
  Futex flow_wait{0};

  /// Current output column (0-based), tracked for ONOCR and tab expansion.
  /// Saturates at 0xFFFF. Does not account for multi-byte codepoint widths.
  uint16_t output_column;

  /// Ready queue: committed bytes available for read(). Circular with offset.
  uint16_t ready_offset;
  uint16_t ready_size;

  /// Canonical editing buffer: accumulates bytes until line-complete or VEOF.
  uint16_t canonical_size;

  unsigned char ready[CONSOLE_TTY_READY_CAPACITY];
  unsigned char canonical[CONSOLE_TTY_CANONICAL_CAPACITY];
};

// ABI validation: catch unintentional growth of this process-resident struct.
// The expected layout on x86_64 is:
//   RawMutex(12) + Atomic<u32>(4) + termios(48) + 2×pid_t(8) + 2×u8(2) +
//   pad(2) + Futex(12) + 4×u16(8) + ready[510] + canonical[255] + pad(1) = 864
// If this fires, audit whether the growth was intentional and update the bound.
static_assert(sizeof(ConsoleTtyState) <= 896,
              "ConsoleTtyState exceeds 896 bytes — accidental field addition?");

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_STATE_H
