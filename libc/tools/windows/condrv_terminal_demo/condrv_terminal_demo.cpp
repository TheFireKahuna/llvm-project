//===-- Interactive ConDrv POSIX terminal demo ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace {

struct DemoState {
  struct termios original = {};
  struct termios raw = {};
  struct winsize window = {};
  bool have_original = false;
  bool raw_enabled = false;
  bool custom_interrupt_handlers = true;
  char number_buffer[16] = {};
  char line_buffer[512] = {};
  char message_buffer[1024] = {};
} g_state;

volatile sig_atomic_t g_pending_sigint = 0;
volatile sig_atomic_t g_pending_sigquit = 0;
volatile sig_atomic_t g_pending_sighup = 0;
volatile sig_atomic_t g_pending_sigterm = 0;
volatile sig_atomic_t g_pending_sigwinch = 0;

static inline size_t cstrlen(const char *str) {
  size_t len = 0;
  while (str[len] != '\0')
    ++len;
  return len;
}

static inline bool write_all(const char *data, size_t length) {
  while (length > 0) {
    ssize_t written = write(1, data, length);
    if (written <= 0)
      return false;
    data += static_cast<size_t>(written);
    length -= static_cast<size_t>(written);
  }
  return true;
}

static inline void write_literal(const char *str) {
  (void)write_all(str, cstrlen(str));
}

static inline char *append_string(char *out, const char *str) {
  while (*str != '\0')
    *out++ = *str++;
  return out;
}

static inline char *append_char(char *out, char c) {
  *out++ = c;
  return out;
}

static inline char *append_u32(char *out, unsigned value) {
  unsigned count = 0;
  do {
    g_state.number_buffer[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  while (count > 0)
    *out++ = g_state.number_buffer[--count];
  return out;
}

static inline char *append_i32(char *out, int value) {
  if (value < 0) {
    *out++ = '-';
    return append_u32(out, static_cast<unsigned>(-value));
  }
  return append_u32(out, static_cast<unsigned>(value));
}

static inline char *append_hex_byte(char *out, unsigned char value) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  *out++ = HEX[(value >> 4) & 0xF];
  *out++ = HEX[value & 0xF];
  return out;
}

static inline char *append_bool(char *out, bool value) {
  return append_string(out, value ? "true" : "false");
}

static inline void flush_message(char *out) {
  *out = '\0';
  write_literal(g_state.message_buffer);
}

static inline bool query_winsize() {
  return ioctl(0, TIOCGWINSZ, &g_state.window) == 0;
}

static inline void print_help() {
  write_literal(
      "\r\n"
      "llvm-libc ConDrv POSIX terminal demo\r\n"
      "  q  quit\r\n"
      "  ?  show this help\r\n"
      "  s  print tty state\r\n"
      "  w  print current winsize\r\n"
      "  m  switch to cooked mode for one line, then return to raw mode\r\n"
      "  f  queue-and-flush input demo\r\n"
      "  b  tcsendbreak(stdout)\r\n"
      "  g  re-assert tcsetpgrp(stdin, tcgetpgrp(stdin))\r\n"
      "  t  toggle SIGINT/SIGQUIT between demo handlers and SIG_DFL\r\n"
      "  Ctrl+L  send VT clear-screen sequence\r\n"
      "  Ctrl+C / Ctrl+\\  exercise signal delivery or default disposition\r\n"
      "\r\n");
}

static inline void print_winsize() {
  char *out = g_state.message_buffer;
  out = append_string(out, "\r\nwinsize: rows=");
  out = append_u32(out, static_cast<unsigned>(g_state.window.ws_row));
  out = append_string(out, " cols=");
  out = append_u32(out, static_cast<unsigned>(g_state.window.ws_col));
  out = append_string(out, " xpixels=");
  out = append_u32(out, static_cast<unsigned>(g_state.window.ws_xpixel));
  out = append_string(out, " ypixels=");
  out = append_u32(out, static_cast<unsigned>(g_state.window.ws_ypixel));
  out = append_string(out, "\r\n");
  flush_message(out);
}

static inline void print_state() {
  (void)query_winsize();
  pid_t tty_sid = tcgetsid(0);
  pid_t fg_pgrp = tcgetpgrp(0);

  char *out = g_state.message_buffer;
  out = append_string(out, "\r\nisatty(stdin)=");
  out = append_bool(out, isatty(0) == 1);
  out = append_string(out, " isatty(stdout)=");
  out = append_bool(out, isatty(1) == 1);
  out = append_string(out, "\r\nraw_enabled=");
  out = append_bool(out, g_state.raw_enabled);
  out = append_string(out, " signal_mode=");
  out = append_string(out, g_state.custom_interrupt_handlers ? "handled"
                                                             : "default");
  out = append_string(out, " tcgetsid=");
  out = append_i32(out, static_cast<int>(tty_sid));
  out = append_string(out, " tcgetpgrp=");
  out = append_i32(out, static_cast<int>(fg_pgrp));
  out = append_string(out, "\r\n");
  flush_message(out);
  print_winsize();
}

static inline void set_signal_flag(int signum) {
  switch (signum) {
  case SIGINT:
    g_pending_sigint = 1;
    break;
  case SIGQUIT:
    g_pending_sigquit = 1;
    break;
  case SIGHUP:
    g_pending_sighup = 1;
    break;
  case SIGTERM:
    g_pending_sigterm = 1;
    break;
  case SIGWINCH:
    g_pending_sigwinch = 1;
    break;
  default:
    break;
  }
}

void handle_signal(int signum) { set_signal_flag(signum); }

static inline void apply_interrupt_signal_mode() {
  if (g_state.custom_interrupt_handlers) {
    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGQUIT, handle_signal);
  } else {
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGQUIT, SIG_DFL);
  }
}

static inline void install_signal_handlers() {
  apply_interrupt_signal_mode();
  (void)signal(SIGHUP, handle_signal);
  (void)signal(SIGTERM, handle_signal);
  (void)signal(SIGWINCH, handle_signal);
}

static inline void toggle_interrupt_signal_mode() {
  g_state.custom_interrupt_handlers = !g_state.custom_interrupt_handlers;
  apply_interrupt_signal_mode();
  write_literal(g_state.custom_interrupt_handlers
                    ? "\r\nsignal mode: handled (demo logs SIGINT/SIGQUIT)\r\n"
                    : "\r\nsignal mode: default (SIGINT/SIGQUIT use SIG_DFL)\r\n");
}

static inline bool enter_raw_mode() {
  if (!g_state.have_original) {
    if (tcgetattr(0, &g_state.original) != 0)
      return false;
    g_state.have_original = true;
  }

  g_state.raw = g_state.original;
  cfmakeraw(&g_state.raw);
  g_state.raw.c_lflag |= ISIG;
  g_state.raw.c_cc[VMIN] = 1;
  g_state.raw.c_cc[VTIME] = 0;

  if (tcsetattr(0, TCSANOW, &g_state.raw) != 0)
    return false;
  g_state.raw_enabled = true;
  return true;
}

static inline void restore_terminal() {
  if (!g_state.have_original)
    return;
  (void)tcsetattr(0, TCSANOW, &g_state.original);
  g_state.raw_enabled = false;
}

static inline void clear_screen() {
  write_literal("\x1b[2J\x1b[H");
}

static inline void print_byte(unsigned char byte) {
  char *out = g_state.message_buffer;
  out = append_string(out, "\r\nbyte: 0x");
  out = append_hex_byte(out, byte);
  out = append_string(out, " ");
  if (byte >= 0x20 && byte <= 0x7E) {
    out = append_char(out, '\'');
    out = append_char(out, static_cast<char>(byte));
    out = append_char(out, '\'');
  } else {
    out = append_string(out, "(control)");
  }
  out = append_string(out, "\r\n");
  flush_message(out);
}

static inline bool handle_pending_signals() {
  if (g_pending_sigwinch) {
    g_pending_sigwinch = 0;
    if (query_winsize())
      print_winsize();
    else
      write_literal("\r\nSIGWINCH received\r\n");
  }
  if (g_pending_sigint) {
    g_pending_sigint = 0;
    write_literal("\r\nSIGINT received\r\n");
  }
  if (g_pending_sigquit) {
    g_pending_sigquit = 0;
    write_literal("\r\nSIGQUIT received\r\n");
  }
  if (g_pending_sighup) {
    g_pending_sighup = 0;
    write_literal("\r\nSIGHUP received, exiting\r\n");
    return false;
  }
  if (g_pending_sigterm) {
    g_pending_sigterm = 0;
    write_literal("\r\nSIGTERM received, exiting\r\n");
    return false;
  }
  return true;
}

static inline void run_cooked_capture() {
  restore_terminal();
  write_literal(
      "\r\n[cooked] type one line and press Enter; the demo will dump the "
      "returned bytes and restore raw mode.\r\n> ");
  ssize_t count = read(0, g_state.line_buffer, sizeof(g_state.line_buffer));
  if (count < 0) {
    char *out = g_state.message_buffer;
    out = append_string(out, "\r\ncooked read failed, errno=");
    out = append_i32(out, errno);
    out = append_string(out, "\r\n");
    flush_message(out);
  } else {
    char *out = g_state.message_buffer;
    out = append_string(out, "\r\ncooked read bytes=");
    out = append_i32(out, static_cast<int>(count));
    out = append_string(out, " data=");
    flush_message(out);
    if (count > 0)
      (void)write_all(g_state.line_buffer, static_cast<size_t>(count));
    write_literal("\r\n");
  }

  if (enter_raw_mode())
    write_literal("[raw] restored\r\n");
}

static inline void run_input_flush_demo() {
  write_literal(
      "\r\n[flush] type ahead for 2 seconds. Keys will not echo during this "
      "window; pending unread input will then be discarded.\r\n");

  struct timespec pause = {};
  pause.tv_sec = 2;
  pause.tv_nsec = 0;
  (void)nanosleep(&pause, nullptr);

  if (tcflush(0, TCIFLUSH) != 0) {
    write_literal("[flush] tcflush(TCIFLUSH) failed\r\n");
    return;
  }

  int old_flags = fcntl(0, F_GETFL);
  if (old_flags < 0) {
    write_literal("[flush] unread input discarded\r\n");
    return;
  }

  if (fcntl(0, F_SETFL, old_flags | O_NONBLOCK) != 0) {
    write_literal("[flush] unread input discarded\r\n");
    return;
  }

  ssize_t residual = 0;
  for (;;) {
    ssize_t count = read(0, g_state.line_buffer, sizeof(g_state.line_buffer));
    if (count > 0) {
      residual += count;
      continue;
    }
    break;
  }

  (void)fcntl(0, F_SETFL, old_flags);

  char *out = g_state.message_buffer;
  out = append_string(out, "[flush] unread input discarded; residual bytes=");
  out = append_i32(out, static_cast<int>(residual));
  out = append_string(out, "\r\n");
  flush_message(out);
}

static inline int interactive_loop() {
  unsigned char byte = 0;
  for (;;) {
    if (!handle_pending_signals())
      return 0;

    ssize_t count = read(0, &byte, 1);
    if (count == 0) {
      write_literal("\r\nEOF on terminal input\r\n");
      return 0;
    }

    if (count < 0) {
      if (errno == EINTR)
        continue;

      char *out = g_state.message_buffer;
      out = append_string(out, "\r\nread failed, errno=");
      out = append_i32(out, errno);
      out = append_string(out, "\r\n");
      flush_message(out);
      return 1;
    }

    switch (byte) {
    case 'q':
      write_literal("\r\nquitting\r\n");
      return 0;
    case '?':
      print_help();
      break;
    case 's':
      print_state();
      break;
    case 'w':
      if (query_winsize())
        print_winsize();
      else
        write_literal("\r\nTIOCGWINSZ failed\r\n");
      break;
    case 'm':
      run_cooked_capture();
      break;
    case 'f':
      run_input_flush_demo();
      break;
    case 'b':
      if (tcsendbreak(1, 0) == 0)
        write_literal("\r\ntcsendbreak(stdout) ok\r\n");
      else
        write_literal("\r\ntcsendbreak(stdout) failed\r\n");
      break;
    case 'g': {
      pid_t foreground_group = tcgetpgrp(0);
      if (tcsetpgrp(0, foreground_group) == 0)
        write_literal("\r\ntcsetpgrp(stdin, tcgetpgrp(stdin)) ok\r\n");
      else
        write_literal("\r\ntcsetpgrp(stdin, tcgetpgrp(stdin)) failed\r\n");
      print_state();
      break;
    }
    case 't':
      toggle_interrupt_signal_mode();
      break;
    case 0x0C:
      clear_screen();
      break;
    default:
      print_byte(byte);
      break;
    }
  }
}

} // namespace

extern "C" int main(int, char **) {
  if (isatty(0) != 1) {
    write_literal(
        "condrv_terminal_demo: run this from an attached terminal/console.\n");
    return 1;
  }

  install_signal_handlers();

  if (!enter_raw_mode()) {
    write_literal("condrv_terminal_demo: failed to enter raw mode.\n");
    return 1;
  }

  print_help();
  print_state();
  int result = interactive_loop();
  restore_terminal();
  return result;
}
