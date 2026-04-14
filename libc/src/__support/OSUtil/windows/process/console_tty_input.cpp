//===-- Console TTY input: canonical editing, signals, read ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements canonical line editing (VERASE/VKILL/VEOF), signal character
// processing (ISIG), VMIN/VTIME non-canonical read semantics, and the public
// console_tty::read() entry point.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/console_tty_impl.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace console_tty {
namespace {

using namespace impl;

// ===----------------------------------------------------------------------===
// Input-specific types and constants
// ===----------------------------------------------------------------------===

enum class WaitResult : uint8_t {
  Ready,
  Timeout,
  Interrupted,
};

struct ProcessedRecord {
  bool data_ready = false;
  bool eof = false;
  bool interrupted = false;
  bool flush_input = false;
  bool should_retry = false;
};

static constexpr unsigned char ERASE_ECHO_SEQUENCE[] = {'\b', ' ', '\b'};
static constexpr unsigned char NEWLINE_ECHO_SEQUENCE[] = {'\r', '\n'};
static constexpr unsigned char BELL_ECHO = '\a';

// ===----------------------------------------------------------------------===
// Buffer manipulation (input-only)
// ===----------------------------------------------------------------------===

void compact_ready_locked(ConsoleTtyState &state) {
  if (state.ready_offset == 0)
    return;
  if (state.ready_offset >= state.ready_size) {
    clear_ready_locked(state);
    return;
  }

  size_t available = ready_available_locked(state);
  __builtin_memmove(state.ready, state.ready + state.ready_offset, available);
  state.ready_offset = 0;
  state.ready_size = static_cast<uint16_t>(available);
}

bool enqueue_ready_locked(ConsoleTtyState &state,
                                      unsigned char byte) {
  compact_ready_locked(state);
  if (state.ready_size >= CONSOLE_TTY_READY_CAPACITY)
    return false;
  state.ready[state.ready_size++] = byte;
  return true;
}

bool commit_canonical_locked(ConsoleTtyState &state) {
  compact_ready_locked(state);
  if (ready_available_locked(state) + state.canonical_size >
      CONSOLE_TTY_READY_CAPACITY)
    return false;

  for (size_t i = 0; i < state.canonical_size; ++i)
    state.ready[state.ready_size++] = state.canonical[i];
  state.canonical_size = 0;
  return true;
}

size_t drain_ready_locked(ConsoleTtyState &state, unsigned char *dst,
                                      size_t max_count) {
  size_t available = ready_available_locked(state);
  if (available == 0 || max_count == 0)
    return 0;

  size_t to_copy = available < max_count ? available : max_count;
  __builtin_memcpy(dst, state.ready + state.ready_offset, to_copy);
  state.ready_offset = static_cast<uint16_t>(state.ready_offset + to_copy);
  if (state.ready_offset == state.ready_size)
    clear_ready_locked(state);
  return to_copy;
}

// ===----------------------------------------------------------------------===
// Control character helpers
// ===----------------------------------------------------------------------===

bool control_char_enabled(const struct termios &attrs,
                                      size_t index) {
  return index < NCCS && attrs.c_cc[index] != _POSIX_VDISABLE;
}

bool matches_erase_control_char(const struct termios &attrs,
                                            unsigned char byte) {
  if (!control_char_enabled(attrs, VERASE))
    return false;

  cc_t erase = attrs.c_cc[VERASE];
  if (byte == erase)
    return true;

  // Terminal emulators commonly surface erase as either BS (^H, 0x08) or
  // DEL (^?, 0x7f). When the configured erase char is one of those canonical
  // values, accept its peer as well so canonical editing remains stable across
  // ConPTY/VT input translation quirks.
  return (erase == static_cast<cc_t>('\b') && byte == 0x7f) ||
         (erase == static_cast<cc_t>(0x7f) && byte == '\b');
}

bool termios_requires_discipline(const struct termios &attrs) {
  if (attrs.c_lflag &
      static_cast<tcflag_t>(ICANON | ISIG | ECHO | ECHOE | ECHOK | ECHONL))
    return true;
  if (attrs.c_iflag &
      static_cast<tcflag_t>(IGNCR | ICRNL | INLCR | IXON | IXOFF | ISTRIP))
    return true;
  return attrs.c_cc[VMIN] != 1 || attrs.c_cc[VTIME] != 0;
}

// ===----------------------------------------------------------------------===
// Input byte translation and signal dispatch
// ===----------------------------------------------------------------------===

int translate_input_byte(const struct termios &attrs,
                                     unsigned char byte) {
  if (attrs.c_iflag & ISTRIP)
    byte &= 0x7F;
  if (byte == '\r') {
    if (attrs.c_iflag & IGNCR)
      return -1;
    if (attrs.c_iflag & ICRNL)
      byte = '\n';
  } else if (byte == '\n' && (attrs.c_iflag & INLCR)) {
    byte = '\r';
  }
  return byte;
}

int signal_for_input_byte(const struct termios &attrs,
                                      unsigned char byte) {
  if (!(attrs.c_lflag & ISIG))
    return 0;
  if (control_char_enabled(attrs, VINTR) && byte == attrs.c_cc[VINTR])
    return SIGINT;
  if (control_char_enabled(attrs, VQUIT) && byte == attrs.c_cc[VQUIT])
    return SIGQUIT;
  if (control_char_enabled(attrs, VSUSP) && byte == attrs.c_cc[VSUSP])
    return SIGTSTP;
  return 0;
}

bool signal_is_ignored(int signum) {
  uint64_t bit = 1ULL << (signum - 1);
  return (g_pcb.signal_handler.ignored.load(cpp::MemoryOrder::ACQUIRE) & bit) !=
         0;
}

bool signal_should_interrupt_terminal_read(int signum) {
  if (signal_is_ignored(signum) || signal_state::is_signal_blocked(signum))
    return false;

  struct sigaction action = signal_state::handler_table::read(signum);
  if (action.sa_handler == SIG_IGN)
    return false;
  if (action.sa_handler != SIG_DFL &&
      (action.sa_flags & static_cast<decltype(action.sa_flags)>(SA_RESTART)) !=
          0)
    return false;
  return true;
}

/// Deliver a terminal signal to the foreground process group. Takes the
/// foreground pgid from the caller's snapshot so no additional state
/// read is needed.
void deliver_terminal_signal(pid_t foreground_pgid, int signum) {
  if (foreground_pgid > 0) {
    intptr_t rc =
        signal_state::kill(-static_cast<intptr_t>(foreground_pgid), signum);
    if (rc == 0 || rc != -ESRCH)
      return;
  }
  signal_state::deliver_process_signal(signum);
}

// ===----------------------------------------------------------------------===
// VMIN/VTIME helpers
// ===----------------------------------------------------------------------===

void arm_vtime_deadline(cc_t vtime, ULONGLONG *deadline_ticks,
                                    bool *deadline_active) {
  if (vtime == 0 || !deadline_ticks || !deadline_active)
    return;

  ULONGLONG now = 0;
  QueryInterruptTime(&now);
  *deadline_ticks =
      now + static_cast<ULONGLONG>(vtime) *
                static_cast<ULONGLONG>(DECISECOND_IN_100NS);
  *deadline_active = true;
}

size_t noncanonical_completion_target(size_t count, cc_t vmin) {
  if (vmin == 0)
    return 0;

  size_t requested = static_cast<size_t>(vmin);
  return requested < count ? requested : count;
}

// ===----------------------------------------------------------------------===
// Echo helpers
// ===----------------------------------------------------------------------===

int ensure_echo_output_handle(windows::ScopedNtHandle &output_handle) {
  if (output_handle.get())
    return 0;
  return open_current_console_output(output_handle);
}

void echo_bytes_locked(ConsoleTtyState &state,
                       const struct termios &attrs,
                       windows::ScopedNtHandle &output_handle,
                       const unsigned char *bytes, size_t count) {
  if (count == 0)
    return;
  if (ensure_echo_output_handle(output_handle) < 0)
    return;
  if (wait_for_output_resume_locked(state) < 0)
    return;
  (void)write_terminal_bytes_locked(state, attrs, output_handle.get(), bytes,
                                    count);
}

void echo_byte_locked(ConsoleTtyState &state,
                      const struct termios &attrs,
                      windows::ScopedNtHandle &output_handle,
                      unsigned char byte) {
  echo_bytes_locked(state, attrs, output_handle, &byte, 1);
}

void echo_repeat_locked(ConsoleTtyState &state,
                        const struct termios &attrs,
                        windows::ScopedNtHandle &output_handle,
                        const unsigned char *bytes, size_t count,
                        size_t repeat) {
  for (size_t i = 0; i < repeat; ++i)
    echo_bytes_locked(state, attrs, output_handle, bytes, count);
}

// ===----------------------------------------------------------------------===
// Canonical editing: VERASE handling
// ===----------------------------------------------------------------------===

ProcessedRecord handle_erase_char(ConsoleTtyState &state,
                                              const struct termios &attrs,
                                              unsigned char byte,
                                              windows::ScopedNtHandle &echo_out) {
  ProcessedRecord result = {};
  if (state.canonical_size != 0) {
    --state.canonical_size;
    if (attrs.c_lflag & ECHOE)
      echo_bytes_locked(state, attrs, echo_out, ERASE_ECHO_SEQUENCE,
                        sizeof(ERASE_ECHO_SEQUENCE));
    else if (attrs.c_lflag & ECHO)
      echo_byte_locked(state, attrs, echo_out, byte);
  }
  result.should_retry = true;
  return result;
}

// ===----------------------------------------------------------------------===
// Canonical editing: VKILL handling
// ===----------------------------------------------------------------------===

ProcessedRecord handle_kill_char(ConsoleTtyState &state,
                                             const struct termios &attrs,
                                             unsigned char byte,
                                             windows::ScopedNtHandle &echo_out) {
  ProcessedRecord result = {};
  size_t killed = state.canonical_size;
  state.canonical_size = 0;
  if (killed != 0 && (attrs.c_lflag & ECHOE))
    echo_repeat_locked(state, attrs, echo_out, ERASE_ECHO_SEQUENCE,
                       sizeof(ERASE_ECHO_SEQUENCE), killed);
  if (attrs.c_lflag & ECHOK)
    echo_bytes_locked(state, attrs, echo_out, NEWLINE_ECHO_SEQUENCE,
                      sizeof(NEWLINE_ECHO_SEQUENCE));
  else if ((attrs.c_lflag & ECHO) != 0)
    echo_byte_locked(state, attrs, echo_out, byte);
  result.should_retry = true;
  return result;
}

// ===----------------------------------------------------------------------===
// Per-byte input state machine
// ===----------------------------------------------------------------------===

ProcessedRecord process_key_byte(ConsoleTtyState &state,
                                             const struct termios &attrs,
                                             pid_t foreground_pgid,
                                             unsigned char byte,
                                             windows::ScopedNtHandle &echo_out) {
  ProcessedRecord result = {};

  int translated = translate_input_byte(attrs, byte);
  if (translated < 0) {
    result.should_retry = true;
    return result;
  }
  byte = static_cast<unsigned char>(translated);

  // XON/XOFF flow control
  if ((attrs.c_iflag & IXON) &&
      ((control_char_enabled(attrs, VSTOP) && byte == attrs.c_cc[VSTOP]) ||
       (control_char_enabled(attrs, VSTART) && byte == attrs.c_cc[VSTART]))) {
    state.output_stopped =
        (control_char_enabled(attrs, VSTOP) && byte == attrs.c_cc[VSTOP]) ? 1
                                                                          : 0;
    notify_flow_state(state);
    result.should_retry = true;
    return result;
  }

  // Signal generation (ISIG)
  int signum = signal_for_input_byte(attrs, byte);
  if (signum != 0) {
    if ((attrs.c_lflag & NOFLSH) == 0) {
      clear_all_buffers_locked(state);
      result.flush_input = true;
    }
    deliver_terminal_signal(foreground_pgid, signum);
    result.interrupted = signal_should_interrupt_terminal_read(signum);
    result.should_retry = !result.interrupted;
    return result;
  }

  // Non-canonical mode: enqueue directly
  if (!(attrs.c_lflag & ICANON)) {
    if (!enqueue_ready_locked(state, byte)) {
      echo_byte_locked(state, attrs, echo_out, BELL_ECHO);
      result.should_retry = true;
      return result;
    }
    if (attrs.c_lflag & ECHO)
      echo_byte_locked(state, attrs, echo_out, byte);
    result.data_ready = true;
    return result;
  }

  // Canonical mode: VERASE
  if (matches_erase_control_char(attrs, byte))
    return handle_erase_char(state, attrs, byte, echo_out);

  // Canonical mode: VKILL
  if (control_char_enabled(attrs, VKILL) && byte == attrs.c_cc[VKILL])
    return handle_kill_char(state, attrs, byte, echo_out);

  // Canonical mode: VEOF
  if (control_char_enabled(attrs, VEOF) && byte == attrs.c_cc[VEOF]) {
    if (state.canonical_size == 0) {
      result.eof = true;
      return result;
    }
    result.data_ready = commit_canonical_locked(state);
    return result;
  }

  // Canonical mode: buffer full
  if (state.canonical_size >= CONSOLE_TTY_CANONICAL_CAPACITY) {
    echo_byte_locked(state, attrs, echo_out, BELL_ECHO);
    result.should_retry = true;
    return result;
  }

  // Canonical mode: normal character
  state.canonical[state.canonical_size++] = byte;
  if ((attrs.c_lflag & ECHO) != 0 ||
      ((attrs.c_lflag & ECHONL) != 0 && byte == '\n')) {
    echo_byte_locked(state, attrs, echo_out, byte);
  }

  bool line_complete = byte == '\n';
  if (!line_complete && control_char_enabled(attrs, VEOL) &&
      byte == attrs.c_cc[VEOL])
    line_complete = true;
  if (!line_complete && control_char_enabled(attrs, VEOL2) &&
      byte == attrs.c_cc[VEOL2])
    line_complete = true;

  if (line_complete)
    result.data_ready = commit_canonical_locked(state);
  else
    result.should_retry = true;

  return result;
}

// ===----------------------------------------------------------------------===
// Input record processing
// ===----------------------------------------------------------------------===

ProcessedRecord
process_input_record(ConsoleTtyState &state, const struct termios &attrs,
                     pid_t foreground_pgid,
                     const condrv::CONSOLE_INPUT_RECORD &record,
                     windows::ScopedNtHandle &echo_out) {
  ProcessedRecord result = {};

  switch (record.EventType) {
  case condrv::WINDOW_BUFFER_SIZE_EVENT: {
    windows::ScopedNtHandle output_handle;
    if (open_current_console_output(output_handle) == 0)
      observe_output_resize(output_handle.get());
    result.should_retry = true;
    return result;
  }
  case condrv::KEY_EVENT:
    break;
  default:
    result.should_retry = true;
    return result;
  }

  const auto &key = record.Event.KeyEvent;
  if (!key.KeyDown || key.RepeatCount == 0) {
    result.should_retry = true;
    return result;
  }

  for (USHORT i = 0; i < key.RepeatCount; ++i) {
    unsigned char byte = static_cast<unsigned char>(key.Character.AsciiChar);
    if (byte == 0)
      continue;

    ProcessedRecord step =
        process_key_byte(state, attrs, foreground_pgid, byte, echo_out);
    result.data_ready = result.data_ready || step.data_ready;
    result.eof = result.eof || step.eof;
    result.interrupted = result.interrupted || step.interrupted;
    result.flush_input = result.flush_input || step.flush_input;
    result.should_retry = result.should_retry || step.should_retry;
    if (step.interrupted || step.eof)
      return result;
  }

  if (!result.data_ready && !result.interrupted && !result.eof)
    result.should_retry = true;
  return result;
}

// ===----------------------------------------------------------------------===
// Console I/O primitives
// ===----------------------------------------------------------------------===

bool is_hangup_status(NTSTATUS status) {
  return status == STATUS_END_OF_FILE || status == STATUS_PIPE_BROKEN ||
         status == STATUS_INVALID_HANDLE || status == STATUS_CANCELLED;
}

int map_hangup_status(NTSTATUS status) {
  deliver_terminal_hangup();
  if (status == STATUS_END_OF_FILE || status == STATUS_PIPE_BROKEN)
    return 0;
  return -windows_util::ntstatus_to_errno(status);
}

int try_read_input_record(HANDLE input_handle,
                                      condrv::CONSOLE_INPUT_RECORD *record) {
  DWORD record_count = 0;
  NTSTATUS status = condrv::read_console_input_ex(
      input_handle, record, 1, &record_count, condrv::CONSOLE_READ_NOWAIT,
      false);
  if (!NT_SUCCESS(status)) {
    if (is_hangup_status(status))
      return map_hangup_status(status);
    return -windows_util::ntstatus_to_errno(status);
  }
  if (record_count == 0)
    return 0;
  return 1;
}

WaitResult wait_for_console_ready(HANDLE input_handle,
                                              bool has_deadline,
                                              ULONGLONG deadline_ticks) {
  for (;;) {
    LARGE_INTEGER *timeout_ptr = nullptr;
    LARGE_INTEGER timeout = {};
    if (has_deadline) {
      ULONGLONG now = 0;
      QueryInterruptTime(&now);
      if (now >= deadline_ticks)
        return WaitResult::Timeout;
      timeout.QuadPart = -static_cast<LONGLONG>(deadline_ticks - now);
      timeout_ptr = &timeout;
    }

    NTSTATUS ws =
        NtWaitForSingleObject(input_handle, /*Alertable=*/TRUE, timeout_ptr);
    if (ws == STATUS_TIMEOUT)
      return WaitResult::Timeout;
    if (ws == STATUS_USER_APC || ws == STATUS_ALERTED) {
      cancel_check();
      auto *tss = signal_state::get_thread_state_noinit();
      if (tss)
        tss->handler_ran = false;
      if (signal_state::should_restart_syscall())
        continue;
      if (tss && !tss->handler_ran)
        continue;
      return WaitResult::Interrupted;
    }
    if (!NT_SUCCESS(ws))
      return WaitResult::Ready;

    switch (consume_pending_resize_events(input_handle)) {
    case PendingInputState::ReadyForRead:
      return WaitResult::Ready;
    case PendingInputState::RetryWait:
    case PendingInputState::WouldBlock:
      continue;
    }
  }
}

// ===----------------------------------------------------------------------===
// Passthrough read (no discipline)
// ===----------------------------------------------------------------------===

ssize_t passthrough_read(OpenFileDescription *ofd, void *buffer,
                                     size_t count) {
  HANDLE input_handle = ofd->handle;

  for (;;) {
    LARGE_INTEGER *timeout_ptr = nullptr;
    LARGE_INTEGER timeout = {};
    if (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK) {
      timeout.QuadPart = 0;
      timeout_ptr = &timeout;
    }

    NTSTATUS ws =
        NtWaitForSingleObject(input_handle, /*Alertable=*/TRUE, timeout_ptr);
    if (ws == STATUS_TIMEOUT)
      return -EAGAIN;
    if (ws == STATUS_USER_APC || ws == STATUS_ALERTED) {
      cancel_check();
      auto *tss = signal_state::get_thread_state_noinit();
      if (tss)
        tss->handler_ran = false;
      if (signal_state::should_restart_syscall())
        continue;
      if (tss && !tss->handler_ran)
        continue;
      return -EINTR;
    }
    if (!NT_SUCCESS(ws))
      return console_file_io::read(input_handle, buffer, count);
    switch (consume_pending_resize_events(input_handle)) {
    case PendingInputState::ReadyForRead:
      break;
    case PendingInputState::RetryWait:
    case PendingInputState::WouldBlock:
      if (timeout_ptr)
        return -EAGAIN;
      continue;
    }
    return console_file_io::read(input_handle, buffer, count);
  }
}

// ===----------------------------------------------------------------------===
// Canonical read path
// ===----------------------------------------------------------------------===

ssize_t read_canonical(ConsoleTtyState &state,
                                   HANDLE input_handle, unsigned char *dst,
                                   size_t count, bool nonblocking) {
  for (;;) {
    state.lock.lock();
    ensure_state_initialized_locked(state);
    TerminalStateSnapshot snap;
    snapshot_terminal_state_locked(state, &snap);

    size_t drained = drain_ready_locked(state, dst, count);
    state.lock.unlock();

    if (drained != 0)
      return static_cast<ssize_t>(drained);

    condrv::CONSOLE_INPUT_RECORD record = {};
    int record_result = try_read_input_record(input_handle, &record);
    if (record_result < 0)
      return record_result;

    if (record_result == 1) {
      windows::ScopedNtHandle echo_output;
      state.lock.lock();
      ensure_state_initialized_locked(state);
      snapshot_terminal_state_locked(state, &snap);
      ProcessedRecord processed =
          process_input_record(state, snap.attrs, snap.foreground_pgid, record,
                               echo_output);
      state.lock.unlock();

      if (processed.flush_input) {
        NTSTATUS status = condrv::flush_console_input(input_handle);
        if (!NT_SUCCESS(status))
          return -windows_util::ntstatus_to_errno(status);
      }

      if (processed.data_ready)
        continue;
      if (processed.eof)
        return 0;
      if (processed.interrupted)
        return -EINTR;
      continue;
    }

    if (nonblocking)
      return -EAGAIN;

    WaitResult wait_result =
        wait_for_console_ready(input_handle, false, 0);
    if (wait_result == WaitResult::Interrupted)
      return -EINTR;
  }
}

// ===----------------------------------------------------------------------===
// Non-canonical read path
// ===----------------------------------------------------------------------===

ssize_t read_noncanonical(ConsoleTtyState &state,
                                      HANDLE input_handle, unsigned char *dst,
                                      size_t count, cc_t vmin, cc_t vtime,
                                      bool nonblocking) {
  size_t total = 0;
  bool have_first_byte = false;
  ULONGLONG deadline_ticks = 0;
  bool deadline_active = false;

  for (;;) {
    state.lock.lock();
    ensure_state_initialized_locked(state);
    TerminalStateSnapshot snap;
    snapshot_terminal_state_locked(state, &snap);

    size_t drained = drain_ready_locked(state, dst + total, count - total);
    total += drained;
    if (drained != 0)
      have_first_byte = true;
    state.lock.unlock();

    if (drained != 0 && vmin != 0 && vtime != 0)
      arm_vtime_deadline(vtime, &deadline_ticks, &deadline_active);

    if (total != 0) {
      if (vmin == 0)
        return static_cast<ssize_t>(total);
      if (total >= noncanonical_completion_target(count, vmin))
        return static_cast<ssize_t>(total);
    } else if (vmin == 0 && vtime == 0) {
      if (nonblocking)
        return -EAGAIN;
      return 0;
    }

    condrv::CONSOLE_INPUT_RECORD record = {};
    int record_result = try_read_input_record(input_handle, &record);
    if (record_result < 0) {
      if (total != 0)
        return static_cast<ssize_t>(total);
      return record_result;
    }

    if (record_result == 1) {
      windows::ScopedNtHandle echo_output;
      state.lock.lock();
      ensure_state_initialized_locked(state);
      snapshot_terminal_state_locked(state, &snap);
      ProcessedRecord processed =
          process_input_record(state, snap.attrs, snap.foreground_pgid, record,
                               echo_output);
      state.lock.unlock();

      if (processed.flush_input) {
        NTSTATUS status = condrv::flush_console_input(input_handle);
        if (!NT_SUCCESS(status)) {
          if (total != 0)
            return static_cast<ssize_t>(total);
          return -windows_util::ntstatus_to_errno(status);
        }
      }

      if (processed.data_ready)
        continue;
      if (processed.eof) {
        if (total != 0)
          return static_cast<ssize_t>(total);
        return 0;
      }
      if (processed.interrupted) {
        if (total != 0)
          return static_cast<ssize_t>(total);
        return -EINTR;
      }
      continue;
    }

    if (nonblocking)
      return total != 0 ? static_cast<ssize_t>(total) : -EAGAIN;

    bool has_deadline = false;
    if (vmin == 0 && vtime != 0) {
      if (!deadline_active)
        arm_vtime_deadline(vtime, &deadline_ticks, &deadline_active);
      has_deadline = true;
    } else if (vmin != 0 && vtime != 0 && have_first_byte) {
      if (!deadline_active)
        arm_vtime_deadline(vtime, &deadline_ticks, &deadline_active);
      has_deadline = true;
    }

    WaitResult wait_result =
        wait_for_console_ready(input_handle, has_deadline, deadline_ticks);
    if (wait_result == WaitResult::Timeout)
      return static_cast<ssize_t>(total);
    if (wait_result == WaitResult::Interrupted) {
      if (total != 0)
        return static_cast<ssize_t>(total);
      return -EINTR;
    }
  }
}

} // anonymous namespace

// ===----------------------------------------------------------------------===
// Public API
// ===----------------------------------------------------------------------===

ssize_t read(OpenFileDescription *ofd, void *buffer, size_t count) {
  if (!ofd || !ofd->is_console())
    return -ENOTTY;
  if (count == 0)
    return 0;

  ConsoleTtyState &state = impl::tty_state();
  HANDLE input_handle = ofd->handle;
  unsigned char *dst = static_cast<unsigned char *>(buffer);

  TerminalStateSnapshot snap = {};
  bool passthrough = false;
  int foreground_err = 0;

  state.lock.lock();
  impl::ensure_state_initialized_locked(state);
  impl::snapshot_terminal_state_locked(state, &snap);
  foreground_err =
      enforce_foreground_access(snap.has_terminal, snap.controlling_sid,
                                snap.foreground_pgid, snap.attrs.c_lflag,
                                TerminalAccessKind::Read);
  passthrough = !termios_requires_discipline(snap.attrs) &&
                impl::ready_available_locked(state) == 0 &&
                state.canonical_size == 0;
  state.lock.unlock();

  if (foreground_err < 0)
    return foreground_err;

  if (passthrough)
    return passthrough_read(ofd, buffer, count);

  int err = impl::apply_console_modes(snap.attrs);
  if (err < 0)
    return err;

  const bool nonblocking =
      (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK) != 0;

  if (snap.attrs.c_lflag & ICANON)
    return read_canonical(state, input_handle, dst, count, nonblocking);

  return read_noncanonical(state, input_handle, dst, count,
                           snap.attrs.c_cc[VMIN], snap.attrs.c_cc[VTIME],
                           nonblocking);
}

void reset_buffered_input() {
  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::clear_all_buffers_locked(state);
  state.lock.unlock();
}

int get_pending_input_bytes(int fd, int *count) {
  int err = impl::validate_console_handle_fd(fd, nullptr);
  if (err < 0)
    return err;

  ConsoleTtyState &state = impl::tty_state();
  state.lock.lock();
  impl::ensure_state_initialized_locked(state);

  // Report bytes already buffered in the ready queue. In canonical mode,
  // the editing buffer is not yet available to read() so exclude it.
  size_t available = impl::ready_available_locked(state);
  state.lock.unlock();

  *count = static_cast<int>(available);
  return 0;
}

} // namespace console_tty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
