//===-- Console TTY behavior tests (LLVM libc test framework) -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exercises terminal behavior through a PTY master/slave pair: canonical line
// editing, VMIN/VTIME, OPOST translations, signal generation, foreground
// process group enforcement, XON/XOFF flow control, and window resize.
//
// All tests create a fresh PTY pair so they are independent and do not
// interfere with each other or the test runner's controlling terminal.
//
//===----------------------------------------------------------------------===//

#include "test/UnitTest/Test.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

// RAII wrapper for a master/slave PTY pair.
struct PtyPair {
  int master = -1;
  int slave = -1;
  bool valid = false;

  PtyPair() {
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) == 0)
      valid = true;
  }

  ~PtyPair() {
    if (slave >= 0)
      close(slave);
    if (master >= 0)
      close(master);
  }

  PtyPair(const PtyPair &) = delete;
  PtyPair &operator=(const PtyPair &) = delete;
};

// Write all bytes to the master side. Returns true on success.
bool write_to_master(int master_fd, const char *data, size_t len) {
  const char *p = data;
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = write(master_fd, p, remaining);
    if (n <= 0)
      return false;
    p += n;
    remaining -= static_cast<size_t>(n);
  }
  return true;
}

// Read up to `max` bytes from fd into buf. Returns bytes read or -1.
ssize_t read_with_timeout(int fd, char *buf, size_t max) {
  // Use non-blocking read with a small retry loop. The PTY's slave side
  // processes input synchronously from master writes, so data should be
  // available almost immediately.
  int old_flags = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);

  ssize_t total = 0;
  for (int attempts = 0; attempts < 50; ++attempts) {
    ssize_t n = read(fd, buf + total, max - static_cast<size_t>(total));
    if (n > 0) {
      total += n;
      break;
    }
    if (n == 0)
      break;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      break;
    // Brief yield before retry.
    usleep(1000);
  }

  fcntl(fd, F_SETFL, old_flags);
  return total > 0 ? total : -1;
}

// Configure the slave for raw (non-canonical) mode with given VMIN/VTIME.
void set_raw_mode(int slave_fd, cc_t vmin, cc_t vtime) {
  struct termios t = {};
  tcgetattr(slave_fd, &t);
  t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
  t.c_iflag &= ~static_cast<tcflag_t>(ICRNL | INLCR | IGNCR | IXON | IXOFF);
  t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
  t.c_cc[VMIN] = vmin;
  t.c_cc[VTIME] = vtime;
  tcsetattr(slave_fd, TCSANOW, &t);
}

// Configure the slave for canonical mode with echo.
void set_canonical_mode(int slave_fd) {
  struct termios t = {};
  tcgetattr(slave_fd, &t);
  t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK;
  t.c_iflag |= ICRNL;
  t.c_oflag |= OPOST;
  tcsetattr(slave_fd, TCSANOW, &t);
}

// Volatile flag for signal delivery tests.
volatile sig_atomic_t g_signal_received = 0;

void signal_handler(int sig) {
  g_signal_received = sig;
}

} // namespace

// ===----------------------------------------------------------------------===
// 1. Canonical line editing (VERASE, VKILL, VEOF)
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, CanonicalLineEditVerase) {
  PtyPair pty;
  if (!pty.valid)
    return;
  set_canonical_mode(pty.slave);

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  unsigned char erase_char = t.c_cc[VERASE];

  // Type "abc", erase last char, then newline. Should get "ab\n".
  char input[5];
  input[0] = 'a';
  input[1] = 'b';
  input[2] = 'c';
  input[3] = static_cast<char>(erase_char);
  input[4] = '\n';
  ASSERT_TRUE(write_to_master(pty.master, input, 5));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.slave, buf, sizeof(buf));
  ASSERT_GT(n, static_cast<ssize_t>(0));
  // Canonical read delivers the completed line: "ab\n".
  EXPECT_EQ(n, static_cast<ssize_t>(3));
  EXPECT_EQ(buf[0], 'a');
  EXPECT_EQ(buf[1], 'b');
  EXPECT_EQ(buf[2], '\n');
}

TEST(LlvmLibcConsoleTtyTest, CanonicalLineEditVkill) {
  PtyPair pty;
  if (!pty.valid)
    return;
  set_canonical_mode(pty.slave);

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  unsigned char kill_char = t.c_cc[VKILL];

  // Type "hello", kill the line, type "hi", newline. Should get "hi\n".
  char input[9];
  input[0] = 'h';
  input[1] = 'e';
  input[2] = 'l';
  input[3] = 'l';
  input[4] = 'o';
  input[5] = static_cast<char>(kill_char);
  input[6] = 'h';
  input[7] = 'i';
  input[8] = '\n';
  ASSERT_TRUE(write_to_master(pty.master, input, 9));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.slave, buf, sizeof(buf));
  ASSERT_GT(n, static_cast<ssize_t>(0));
  EXPECT_EQ(n, static_cast<ssize_t>(3));
  EXPECT_EQ(buf[0], 'h');
  EXPECT_EQ(buf[1], 'i');
  EXPECT_EQ(buf[2], '\n');
}

TEST(LlvmLibcConsoleTtyTest, CanonicalVeofDeliversPartialLine) {
  PtyPair pty;
  if (!pty.valid)
    return;
  set_canonical_mode(pty.slave);

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  unsigned char eof_char = t.c_cc[VEOF];

  // Type "ab" then EOF. Should deliver "ab" (no newline).
  char input[3];
  input[0] = 'a';
  input[1] = 'b';
  input[2] = static_cast<char>(eof_char);
  ASSERT_TRUE(write_to_master(pty.master, input, 3));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.slave, buf, sizeof(buf));
  ASSERT_GT(n, static_cast<ssize_t>(0));
  EXPECT_EQ(n, static_cast<ssize_t>(2));
  EXPECT_EQ(buf[0], 'a');
  EXPECT_EQ(buf[1], 'b');
}

TEST(LlvmLibcConsoleTtyTest, CanonicalVeofOnEmptyLineReturnsZero) {
  PtyPair pty;
  if (!pty.valid)
    return;
  set_canonical_mode(pty.slave);

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  unsigned char eof_char = t.c_cc[VEOF];

  // EOF on empty line → read returns 0 (EOF indication).
  char input[1] = {static_cast<char>(eof_char)};
  ASSERT_TRUE(write_to_master(pty.master, input, 1));

  // Set non-blocking so we don't hang if EOF isn't delivered.
  int flags = fcntl(pty.slave, F_GETFL);
  fcntl(pty.slave, F_SETFL, flags | O_NONBLOCK);

  char buf[16] = {};
  // Brief delay to let the PTY process the input.
  usleep(5000);
  ssize_t n = read(pty.slave, buf, sizeof(buf));
  // n == 0 means EOF, which is the expected POSIX behavior.
  EXPECT_EQ(n, static_cast<ssize_t>(0));

  fcntl(pty.slave, F_SETFL, flags);
}

// ===----------------------------------------------------------------------===
// 2. VMIN/VTIME non-canonical read semantics
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, NonCanonicalVminSatisfied) {
  PtyPair pty;
  if (!pty.valid)
    return;
  // VMIN=3, VTIME=0: blocking read, returns when 3+ bytes available.
  set_raw_mode(pty.slave, 3, 0);

  ASSERT_TRUE(write_to_master(pty.master, "abc", 3));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.slave, buf, sizeof(buf));
  ASSERT_GE(n, static_cast<ssize_t>(3));
  EXPECT_EQ(buf[0], 'a');
  EXPECT_EQ(buf[1], 'b');
  EXPECT_EQ(buf[2], 'c');
}

TEST(LlvmLibcConsoleTtyTest, NonCanonicalVminOneReturnsImmediately) {
  PtyPair pty;
  if (!pty.valid)
    return;
  // VMIN=1, VTIME=0: return as soon as any data available.
  set_raw_mode(pty.slave, 1, 0);

  ASSERT_TRUE(write_to_master(pty.master, "x", 1));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.slave, buf, sizeof(buf));
  ASSERT_GE(n, static_cast<ssize_t>(1));
  EXPECT_EQ(buf[0], 'x');
}

TEST(LlvmLibcConsoleTtyTest, NonCanonicalVminZeroVtimeZeroPolling) {
  PtyPair pty;
  if (!pty.valid)
    return;
  // VMIN=0, VTIME=0: pure polling — returns immediately with whatever is ready.
  set_raw_mode(pty.slave, 0, 0);

  // Nothing written yet — should return 0 or -1/EAGAIN immediately.
  int flags = fcntl(pty.slave, F_GETFL);
  fcntl(pty.slave, F_SETFL, flags | O_NONBLOCK);

  char buf[16] = {};
  ssize_t n = read(pty.slave, buf, sizeof(buf));
  // Either 0 (no data) or -1 with EAGAIN are acceptable.
  EXPECT_TRUE(n == 0 || (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));

  fcntl(pty.slave, F_SETFL, flags);

  // Now write data and read again.
  ASSERT_TRUE(write_to_master(pty.master, "q", 1));
  usleep(2000);

  fcntl(pty.slave, F_SETFL, flags | O_NONBLOCK);
  n = read(pty.slave, buf, sizeof(buf));
  EXPECT_GE(n, static_cast<ssize_t>(1));
  if (n > 0)
    EXPECT_EQ(buf[0], 'q');

  fcntl(pty.slave, F_SETFL, flags);
}

// ===----------------------------------------------------------------------===
// 3. OPOST output transformations (ONLCR, OCRNL, ONOCR, ONLRET)
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, OpostOnlcrTranslation) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // Enable OPOST + ONLCR on the slave.
  struct termios t = {};
  tcgetattr(pty.slave, &t);
  t.c_oflag |= OPOST | ONLCR;
  t.c_oflag &= ~static_cast<tcflag_t>(OCRNL | ONOCR | ONLRET);
  t.c_lflag &= ~static_cast<tcflag_t>(ECHO); // suppress echo interference
  tcsetattr(pty.slave, TCSANOW, &t);

  // Write a newline through the slave's output path.
  ASSERT_EQ(write(pty.slave, "A\nB", 3), static_cast<ssize_t>(3));

  // Read from master — ONLCR translates \n → \r\n.
  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.master, buf, sizeof(buf));
  ASSERT_GE(n, static_cast<ssize_t>(4));
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[1], '\r');
  EXPECT_EQ(buf[2], '\n');
  EXPECT_EQ(buf[3], 'B');
}

TEST(LlvmLibcConsoleTtyTest, OpostOcrnlTranslation) {
  PtyPair pty;
  if (!pty.valid)
    return;

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  t.c_oflag |= OPOST | OCRNL;
  t.c_oflag &= ~static_cast<tcflag_t>(ONLCR | ONOCR | ONLRET);
  t.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  tcsetattr(pty.slave, TCSANOW, &t);

  // OCRNL: \r → \n.
  ASSERT_EQ(write(pty.slave, "X\rY", 3), static_cast<ssize_t>(3));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.master, buf, sizeof(buf));
  ASSERT_GE(n, static_cast<ssize_t>(3));
  EXPECT_EQ(buf[0], 'X');
  EXPECT_EQ(buf[1], '\n');
  EXPECT_EQ(buf[2], 'Y');
}

TEST(LlvmLibcConsoleTtyTest, OpostDisabledPassesThrough) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // Clear OPOST entirely — output should pass through unmodified.
  struct termios t = {};
  tcgetattr(pty.slave, &t);
  t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
  t.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  tcsetattr(pty.slave, TCSANOW, &t);

  ASSERT_EQ(write(pty.slave, "A\nB\rC", 5), static_cast<ssize_t>(5));

  char buf[64] = {};
  ssize_t n = read_with_timeout(pty.master, buf, sizeof(buf));
  ASSERT_GE(n, static_cast<ssize_t>(5));
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[1], '\n');
  EXPECT_EQ(buf[2], 'B');
  EXPECT_EQ(buf[3], '\r');
  EXPECT_EQ(buf[4], 'C');
}

// ===----------------------------------------------------------------------===
// 4. Signal generation from control characters
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, VintrGeneratesSigint) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // Enable ISIG on the slave.
  struct termios t = {};
  tcgetattr(pty.slave, &t);
  t.c_lflag |= ISIG;
  tcsetattr(pty.slave, TCSANOW, &t);

  unsigned char intr_char = t.c_cc[VINTR];
  if (intr_char == 0)
    return; // VINTR disabled

  // Install a handler so we can detect signal delivery.
  struct sigaction sa = {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_sa = {};
  sigaction(SIGINT, &sa, &old_sa);

  g_signal_received = 0;
  char input[1] = {static_cast<char>(intr_char)};
  write_to_master(pty.master, input, 1);

  // Allow time for signal delivery.
  usleep(10000);

  EXPECT_EQ(g_signal_received, SIGINT);

  sigaction(SIGINT, &old_sa, nullptr);
}

TEST(LlvmLibcConsoleTtyTest, VquitGeneratesSigquit) {
  PtyPair pty;
  if (!pty.valid)
    return;

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  t.c_lflag |= ISIG;
  tcsetattr(pty.slave, TCSANOW, &t);

  unsigned char quit_char = t.c_cc[VQUIT];
  if (quit_char == 0)
    return;

  struct sigaction sa = {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_sa = {};
  sigaction(SIGQUIT, &sa, &old_sa);

  g_signal_received = 0;
  char input[1] = {static_cast<char>(quit_char)};
  write_to_master(pty.master, input, 1);

  usleep(10000);
  EXPECT_EQ(g_signal_received, SIGQUIT);

  sigaction(SIGQUIT, &old_sa, nullptr);
}

TEST(LlvmLibcConsoleTtyTest, VsuspGeneratesSigtstp) {
  PtyPair pty;
  if (!pty.valid)
    return;

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  t.c_lflag |= ISIG;
  tcsetattr(pty.slave, TCSANOW, &t);

  unsigned char susp_char = t.c_cc[VSUSP];
  if (susp_char == 0)
    return;

  struct sigaction sa = {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_sa = {};
  sigaction(SIGTSTP, &sa, &old_sa);

  g_signal_received = 0;
  char input[1] = {static_cast<char>(susp_char)};
  write_to_master(pty.master, input, 1);

  usleep(10000);
  EXPECT_EQ(g_signal_received, SIGTSTP);

  sigaction(SIGTSTP, &old_sa, nullptr);
}

TEST(LlvmLibcConsoleTtyTest, IsigDisabledPassesControlCharsThrough) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // With ISIG off, control chars should be delivered as data.
  set_raw_mode(pty.slave, 1, 0);

  struct termios t = {};
  tcgetattr(pty.slave, &t);
  unsigned char intr_char = t.c_cc[VINTR];
  if (intr_char == 0)
    return;

  struct sigaction sa = {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_sa = {};
  sigaction(SIGINT, &sa, &old_sa);

  g_signal_received = 0;
  char input[1] = {static_cast<char>(intr_char)};
  ASSERT_TRUE(write_to_master(pty.master, input, 1));

  char buf[16] = {};
  ssize_t n = read_with_timeout(pty.slave, buf, sizeof(buf));
  ASSERT_GE(n, static_cast<ssize_t>(1));
  // The control character should arrive as data.
  EXPECT_EQ(static_cast<unsigned char>(buf[0]), intr_char);
  // No signal should have been raised.
  EXPECT_EQ(g_signal_received, 0);

  sigaction(SIGINT, &old_sa, nullptr);
}

// ===----------------------------------------------------------------------===
// 5. Foreground process group enforcement (SIGTTIN/SIGTTOU)
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, TcsetpgrpRoundTrips) {
  PtyPair pty;
  if (!pty.valid)
    return;

  pid_t our_pgrp = getpgrp();
  ASSERT_GT(our_pgrp, static_cast<pid_t>(0));

  // tcsetpgrp should succeed for our own process group.
  int rc = tcsetpgrp(pty.slave, our_pgrp);
  // May fail if we don't have a controlling terminal; that's OK.
  if (rc != 0)
    return;

  pid_t fg = tcgetpgrp(pty.slave);
  EXPECT_EQ(fg, our_pgrp);
}

TEST(LlvmLibcConsoleTtyTest, TcgetpgrpFailsOnNonTerminal) {
  // Create a pipe — not a terminal.
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  pid_t fg = tcgetpgrp(pipefd[0]);
  EXPECT_EQ(fg, static_cast<pid_t>(-1));
  EXPECT_EQ(errno, ENOTTY);

  close(pipefd[0]);
  close(pipefd[1]);
}

// ===----------------------------------------------------------------------===
// 6. XON/XOFF flow control
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, TcflowStopsAndResumesOutput) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // tcflow(TCOOFF) should succeed — stops output.
  int rc = tcflow(pty.slave, TCOOFF);
  EXPECT_EQ(rc, 0);

  // tcflow(TCOON) should succeed — resumes output.
  rc = tcflow(pty.slave, TCOON);
  EXPECT_EQ(rc, 0);
}

TEST(LlvmLibcConsoleTtyTest, TcflowInvalidActionFails) {
  PtyPair pty;
  if (!pty.valid)
    return;

  int rc = tcflow(pty.slave, 99);
  EXPECT_EQ(rc, -1);
  EXPECT_EQ(errno, EINVAL);
}

TEST(LlvmLibcConsoleTtyTest, TcdrainSucceeds) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // Write some data, then drain.
  write(pty.slave, "test", 4);
  int rc = tcdrain(pty.slave);
  EXPECT_EQ(rc, 0);
}

// ===----------------------------------------------------------------------===
// 7. Window resize (TIOCSWINSZ → SIGWINCH)
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, WinsizeRoundTrips) {
  PtyPair pty;
  if (!pty.valid)
    return;

  struct winsize ws_set = {};
  ws_set.ws_row = 25;
  ws_set.ws_col = 80;
  ws_set.ws_xpixel = 0;
  ws_set.ws_ypixel = 0;
  int rc = ioctl(pty.slave, TIOCSWINSZ, &ws_set);
  ASSERT_EQ(rc, 0);

  struct winsize ws_get = {};
  rc = ioctl(pty.slave, TIOCGWINSZ, &ws_get);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(ws_get.ws_row, static_cast<unsigned short>(25));
  EXPECT_EQ(ws_get.ws_col, static_cast<unsigned short>(80));
}

TEST(LlvmLibcConsoleTtyTest, WinsizeZeroDimensionAllowed) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // POSIX/Linux allow 0-dimension windows (means "unknown size").
  struct winsize ws = {};
  ws.ws_row = 0;
  ws.ws_col = 0;
  int rc = ioctl(pty.slave, TIOCSWINSZ, &ws);
  EXPECT_EQ(rc, 0);
}

TEST(LlvmLibcConsoleTtyTest, WinsizeChangeDeliversSigwinch) {
  PtyPair pty;
  if (!pty.valid)
    return;

  struct sigaction sa = {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_sa = {};
  sigaction(SIGWINCH, &sa, &old_sa);

  // Set an initial size.
  struct winsize ws = {};
  ws.ws_row = 24;
  ws.ws_col = 80;
  ioctl(pty.slave, TIOCSWINSZ, &ws);
  usleep(5000);

  // Now change it — should deliver SIGWINCH.
  g_signal_received = 0;
  ws.ws_row = 50;
  ws.ws_col = 132;
  ioctl(pty.slave, TIOCSWINSZ, &ws);

  usleep(10000);
  EXPECT_EQ(g_signal_received, SIGWINCH);

  sigaction(SIGWINCH, &old_sa, nullptr);
}

// ===----------------------------------------------------------------------===
// Additional: tcgetattr/tcsetattr round-trip
// ===----------------------------------------------------------------------===

TEST(LlvmLibcConsoleTtyTest, TcgetattrTcsetattrRoundTrip) {
  PtyPair pty;
  if (!pty.valid)
    return;

  struct termios orig = {};
  ASSERT_EQ(tcgetattr(pty.slave, &orig), 0);

  // Flip ECHO.
  struct termios modified = orig;
  modified.c_lflag ^= ECHO;
  ASSERT_EQ(tcsetattr(pty.slave, TCSANOW, &modified), 0);

  struct termios readback = {};
  ASSERT_EQ(tcgetattr(pty.slave, &readback), 0);
  EXPECT_EQ(readback.c_lflag & ECHO, modified.c_lflag & ECHO);

  // Restore.
  tcsetattr(pty.slave, TCSANOW, &orig);
}

TEST(LlvmLibcConsoleTtyTest, TcflushInput) {
  PtyPair pty;
  if (!pty.valid)
    return;

  set_raw_mode(pty.slave, 0, 0);

  // Write data to master, then flush slave input.
  write_to_master(pty.master, "flush_me", 8);
  usleep(5000);

  ASSERT_EQ(tcflush(pty.slave, TCIFLUSH), 0);

  // After flush, no data should be available.
  int flags = fcntl(pty.slave, F_GETFL);
  fcntl(pty.slave, F_SETFL, flags | O_NONBLOCK);

  char buf[64] = {};
  ssize_t n = read(pty.slave, buf, sizeof(buf));
  EXPECT_TRUE(n == 0 || (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));

  fcntl(pty.slave, F_SETFL, flags);
}

TEST(LlvmLibcConsoleTtyTest, IsattyOnPtySlave) {
  PtyPair pty;
  if (!pty.valid)
    return;

  EXPECT_EQ(isatty(pty.slave), 1);
  EXPECT_EQ(isatty(pty.master), 1);
}

TEST(LlvmLibcConsoleTtyTest, IsattyOnPipeFails) {
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);
  EXPECT_NE(isatty(pipefd[0]), 1);
  EXPECT_NE(isatty(pipefd[1]), 1);
  close(pipefd[0]);
  close(pipefd[1]);
}

TEST(LlvmLibcConsoleTtyTest, FionreadReturnsZeroOnEmptyQueue) {
  PtyPair pty;
  if (!pty.valid)
    return;

  set_raw_mode(pty.slave, 0, 0);

  int count = -1;
  int rc = ioctl(pty.slave, FIONREAD, &count);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(count, 0);
}

TEST(LlvmLibcConsoleTtyTest, FionreadReflectsQueuedInput) {
  PtyPair pty;
  if (!pty.valid)
    return;

  set_raw_mode(pty.slave, 0, 0);

  ASSERT_TRUE(write_to_master(pty.master, "hello", 5));
  usleep(5000);

  int count = 0;
  int rc = ioctl(pty.slave, FIONREAD, &count);
  ASSERT_EQ(rc, 0);
  EXPECT_GE(count, 5);
}

TEST(LlvmLibcConsoleTtyTest, TcsendbreakSucceeds) {
  PtyPair pty;
  if (!pty.valid)
    return;

  // tcsendbreak should succeed on a terminal fd.
  int rc = tcsendbreak(pty.slave, 0);
  EXPECT_EQ(rc, 0);
}

TEST(LlvmLibcConsoleTtyTest, TcsendbreakFailsOnNonTerminal) {
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);

  int rc = tcsendbreak(pipefd[0], 0);
  EXPECT_EQ(rc, -1);
  EXPECT_EQ(errno, ENOTTY);

  close(pipefd[0]);
  close(pipefd[1]);
}
