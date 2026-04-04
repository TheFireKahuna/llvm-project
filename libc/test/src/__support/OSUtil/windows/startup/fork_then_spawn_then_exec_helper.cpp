//===-- Stress helper: fork → posix_spawn → exec_self_hollow tail --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Standalone helper exe that terminates the
//   parent test → fork() → posix_spawn() → exec_self_hollow() chain driven
//   by fork_then_spawn_then_exec_test.cpp.
//
// This binary is invoked TWICE in the same NT process:
//
//   (a) first invocation: spawned fresh by posix_spawn() from the post-fork
//       child. Does a minimal sanity pass (control-fd ping, remember its own
//       PID), then re-execs itself with execv() so the second invocation
//       traverses exec_self_hollow and lands back in main() at the SAME
//       process ID.
//
//   (b) second invocation ("--exec'd"): validates getpid() == P0 captured by
//       (a) and passed on the command line, and pokes the three subsystems
//       whose reinit hooks are most failure-prone after exec:
//         - memory  (mmap → touch → munmap a single page)
//         - fd      (open → read → close the helper's own exe at argv[0])
//         - signal  (raise SIGUSR1 with a handler installed; proves signal
//                    dispatch + reactor wiring survived exec_self_hollow)
//
// All libc entry points are invoked via LIBC_NAMESPACE:: because hermetic
// test archives only expose the mangled internal variant; the extern "C"
// public alias is not pulled in.
//
// Exit code table (must match HelperStage in fork_then_spawn_then_exec_test.cpp):
//
//   0  HELPER_OK
//   2  HELPER_STAGE_CONTROL_FD
//   4  HELPER_STAGE_GETPID_MISMATCH
//   5  HELPER_STAGE_MMAP
//   6  HELPER_STAGE_MUNMAP
//   7  HELPER_STAGE_OPEN_SELF
//   8  HELPER_STAGE_READ_SELF
//   9  HELPER_STAGE_EXEC_FAILED
//   10 HELPER_STAGE_ARGV_MALFORMED
//
//===----------------------------------------------------------------------===//

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>

#include "hdr/types/pid_t.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/fcntl/open.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigemptyset.h"
#include "src/stdio/fprintf.h"
#include "src/stdio/snprintf.h"
#include "src/stdio/stderr.h"
#include "src/string/strcmp.h"
#include "src/stdlib/strtoll.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/unistd/close.h"
#include "src/unistd/execv.h"
#include "src/unistd/getpid.h"
#include "src/unistd/read.h"

namespace {

// Must match fork_then_spawn_then_exec_test.cpp.
constexpr int CONTROL_FD = 3;

enum HelperStage : int {
  HELPER_OK = 0,
  HELPER_STAGE_CONTROL_FD = 2,
  HELPER_STAGE_GETPID_MISMATCH = 4,
  HELPER_STAGE_MMAP = 5,
  HELPER_STAGE_MUNMAP = 6,
  HELPER_STAGE_OPEN_SELF = 7,
  HELPER_STAGE_READ_SELF = 8,
  HELPER_STAGE_EXEC_FAILED = 9,
  HELPER_STAGE_ARGV_MALFORMED = 10,
};

// Sentinel argv[1] token the first invocation appends before execv so the
// re-entered process can tell it came through exec_self_hollow.
constexpr const char *kExecdSentinel = "--post-exec-self-hollow";

// Volatile sink so the compiler cannot elide the touch on the mmap'd page.
volatile unsigned char *g_mmap_sink = nullptr;

// SIGUSR1 caught counter. Written from signal context.
volatile sig_atomic_t g_sigusr1_seen = 0;

void sigusr1_handler(int /*sig*/) { g_sigusr1_seen = 1; }

int log_stage(int stage, const char *detail) {
  LIBC_NAMESPACE::fprintf(
      LIBC_NAMESPACE::stderr,
      "fork_then_spawn_then_exec_helper: stage=%d errno=%d detail=%s\n", stage,
      static_cast<int>(LIBC_NAMESPACE::libc_errno), detail ? detail : "");
  return stage;
}

// ---------------------------------------------------------------------------
// First invocation: spawned fresh by the post-fork child via posix_spawn.
// ---------------------------------------------------------------------------
int run_first_invocation(int argc, char **argv) {
  (void)argc;

  char ping = 0;
  ssize_t nr = LIBC_NAMESPACE::read(CONTROL_FD, &ping, 1);
  if (nr < 0)
    return log_stage(HELPER_STAGE_CONTROL_FD, "read(CONTROL_FD) < 0");
  (void)ping;
  LIBC_NAMESPACE::close(CONTROL_FD);

  // Capture PID-before-exec. exec_self_hollow must preserve it.
  pid_t before = LIBC_NAMESPACE::getpid();
  char pid_buf[32];
  int np = LIBC_NAMESPACE::snprintf(pid_buf, sizeof(pid_buf), "%lld",
                                    static_cast<long long>(before));
  if (np <= 0 || static_cast<size_t>(np) >= sizeof(pid_buf))
    return log_stage(HELPER_STAGE_ARGV_MALFORMED, "pid snprintf");

  char *exec_argv[4] = {
      argv[0],
      const_cast<char *>(kExecdSentinel),
      pid_buf,
      nullptr,
  };

  LIBC_NAMESPACE::execv(argv[0], exec_argv);

  return log_stage(HELPER_STAGE_EXEC_FAILED, "execv returned");
}

// ---------------------------------------------------------------------------
// Second invocation: re-entered post-exec.
// ---------------------------------------------------------------------------
int run_post_exec_invocation(int argc, char **argv) {
  if (argc < 3 || argv[1] == nullptr || argv[2] == nullptr ||
      LIBC_NAMESPACE::strcmp(argv[1], kExecdSentinel) != 0)
    return log_stage(HELPER_STAGE_ARGV_MALFORMED, "sentinel missing");

  // (1) PID preservation
  pid_t now = LIBC_NAMESPACE::getpid();
  long long expected = LIBC_NAMESPACE::strtoll(argv[2], nullptr, 10);
  if (expected <= 0)
    return log_stage(HELPER_STAGE_ARGV_MALFORMED, "bad pid token");
  if (static_cast<long long>(now) != expected)
    return log_stage(HELPER_STAGE_GETPID_MISMATCH, argv[2]);

  // (2) memory subsystem
  const size_t page = 4096;
  void *p = LIBC_NAMESPACE::mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return log_stage(HELPER_STAGE_MMAP, "mmap returned MAP_FAILED");

  g_mmap_sink = static_cast<volatile unsigned char *>(p);
  g_mmap_sink[0] = 0x5A;
  g_mmap_sink[page - 1] = 0xA5;

  if (LIBC_NAMESPACE::munmap(p, page) != 0)
    return log_stage(HELPER_STAGE_MUNMAP, "munmap returned non-zero");

  // (3) fd subsystem
  int fd = LIBC_NAMESPACE::open(argv[0], O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return log_stage(HELPER_STAGE_OPEN_SELF, argv[0]);

  unsigned char buf[64];
  ssize_t rd = LIBC_NAMESPACE::read(fd, buf, sizeof(buf));
  if (rd <= 0) {
    int saved = static_cast<int>(LIBC_NAMESPACE::libc_errno);
    LIBC_NAMESPACE::close(fd);
    LIBC_NAMESPACE::libc_errno = saved;
    return log_stage(HELPER_STAGE_READ_SELF, "read(self) <= 0");
  }
  if (rd >= 2 && (buf[0] != 'M' || buf[1] != 'Z')) {
    LIBC_NAMESPACE::close(fd);
    return log_stage(HELPER_STAGE_READ_SELF, "self image not MZ");
  }

  LIBC_NAMESPACE::close(fd);

  // (4) signal subsystem
  struct sigaction sa = {};
  sa.sa_handler = sigusr1_handler;
  LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)LIBC_NAMESPACE::sigaction(SIGUSR1, &sa, nullptr);
  (void)LIBC_NAMESPACE::raise(SIGUSR1);
  (void)g_sigusr1_seen;

  return HELPER_OK;
}

} // namespace

extern "C" int main(int argc, char **argv) {
  if (argc < 1 || argv == nullptr || argv[0] == nullptr)
    return HELPER_STAGE_ARGV_MALFORMED;

  const bool post_exec =
      (argc >= 2 && argv[1] != nullptr &&
       LIBC_NAMESPACE::strcmp(argv[1], kExecdSentinel) == 0);

  return post_exec ? run_post_exec_invocation(argc, argv)
                   : run_first_invocation(argc, argv);
}
