//===-- ConDrv-backed tty policy helpers -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_H

#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "hdr/types/struct_winsize.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/process/console_handle_utils.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

struct termios;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct OpenFileDescription;

namespace console_tty {

enum class PendingInputState : uint8_t {
  ReadyForRead,
  RetryWait,
  WouldBlock,
};

// Returns 0 when fd participates in our terminal/termios model, -errno
// otherwise. This is the shared capability predicate that higher-level POSIX
// terminal entrypoints should agree on.
int is_terminal_fd(int fd);

int validate_console_fd(int fd, OpenFileDescription **ofd_out = nullptr);
int get_attr(int fd, struct termios *t);
int set_attr(int fd, int actions, const struct termios *t);
int flush(int fd, int queue_selector);
int drain(int fd);
ErrorOr<pid_t> get_sid(int fd);
ErrorOr<pid_t> get_foreground_pgrp(int fd);
int set_foreground_pgrp(int fd, pid_t pgid);
int flow(int fd, int action);
int send_break(int fd, int duration);
int get_pending_input_bytes(int fd, int *count);
int get_winsize(int fd, struct winsize *ws);
int set_winsize(int fd, const struct winsize *ws);
ssize_t read(OpenFileDescription *ofd, void *buffer, size_t count,
             HANDLE io_target = nullptr);
ssize_t write(OpenFileDescription *ofd, const void *buffer, size_t count);
void reset_buffered_input();
void adopt_controlling_terminal();
void detach_controlling_terminal();

LIBC_INLINE void reset_terminal_signal_state() {
  auto &transport = g_pcb.signal_transport;
  transport.tty_winsize.store(0, cpp::MemoryOrder::RELAXED);
  transport.tty_winsize_initialized.store(0, cpp::MemoryOrder::RELAXED);
  transport.tty_hangup_sent.store(0, cpp::MemoryOrder::RELEASE);
}

LIBC_INLINE void deliver_terminal_hangup() {
  auto &transport = g_pcb.signal_transport;
  uint32_t expected = 0;
  if (!transport.tty_hangup_sent.compare_exchange_strong(
          expected, 1, cpp::MemoryOrder::ACQ_REL))
    return;
  signal_state::deliver_process_signal(SIGHUP);
}

LIBC_INLINE uint16_t clamp_winsize_component(ULONG value) {
  return value > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(value);
}

LIBC_INLINE uint32_t pack_winsize(uint16_t rows, uint16_t cols) {
  return (static_cast<uint32_t>(cols) << 16) | rows;
}

LIBC_INLINE void observe_winsize_components(uint16_t rows, uint16_t cols,
                                            bool signal_on_change) {
  uint32_t packed = pack_winsize(rows, cols);
  auto &transport = g_pcb.signal_transport;

  if (transport.tty_winsize_initialized.load(cpp::MemoryOrder::ACQUIRE) == 0) {
    transport.tty_winsize.store(packed, cpp::MemoryOrder::RELAXED);
    transport.tty_winsize_initialized.store(1, cpp::MemoryOrder::RELEASE);
    return;
  }

  uint32_t previous =
      transport.tty_winsize.exchange(packed, cpp::MemoryOrder::ACQ_REL);
  if (signal_on_change && previous != packed)
    signal_state::deliver_process_signal(SIGWINCH);
}

LIBC_INLINE void observe_output_resize(HANDLE output_handle) {
  if (!output_handle)
    return;

  condrv::CONSOLE_SCREEN_BUFFER_INFO_EX info = {};
  NTSTATUS status =
      condrv::get_console_screen_buffer_info_ex(output_handle, &info);
  if (!NT_SUCCESS(status))
    return;

  uint16_t rows = 0, cols = 0;
  console_util::srwindow_to_rowcol(info.srWindow, &rows, &cols);
  observe_winsize_components(rows, cols, true);
}

LIBC_INLINE PendingInputState consume_pending_resize_events(HANDLE input_handle) {
  if (!input_handle)
    return PendingInputState::ReadyForRead;

  bool saw_resize = false;
  for (;;) {
    condrv::CONSOLE_INPUT_RECORD record = {};
    DWORD record_count = 0;
    NTSTATUS status =
        condrv::peek_console_input(input_handle, &record, 1, &record_count);
    if (!NT_SUCCESS(status) || record_count == 0)
      break;
    if (record.EventType != condrv::WINDOW_BUFFER_SIZE_EVENT)
      return PendingInputState::ReadyForRead;

    status = condrv::read_console_input(input_handle, &record, 1, &record_count);
    if (!NT_SUCCESS(status) || record_count == 0)
      break;
    saw_resize = true;
  }

  if (!saw_resize)
    return PendingInputState::WouldBlock;

  windows::ScopedNtHandle output_handle;
  HANDLE raw_output = nullptr;
  NTSTATUS status = condrv::open_condrv_absolute(
      &raw_output, condrv::CONDRV_CURRENT_OUTPUT_PATH,
      FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE);
  if (NT_SUCCESS(status))
    output_handle.reset(raw_output);
  observe_output_resize(output_handle.get());
  return PendingInputState::RetryWait;
}

} // namespace console_tty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_TTY_H
