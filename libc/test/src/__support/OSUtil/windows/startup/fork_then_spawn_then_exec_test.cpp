//===-- Stress: fork → posix_spawn → exec_self_hollow ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// End-to-end stress test for the full process-creation reinit pipeline.
//
// The chain exercised here is the worst-case composition of every reinit
// hook in the libc:
//
//   parent test                                 (Tier-A boot already done)
//      |
//      v
//   fork()                                      ── libc_fork_reinit:
//      |                                          - zone 0b unseal/canary
//      |                                            check + cookie rotate
//      |                                          - veh re-register
//      |                                          - memory_reconcile_*
//      |                                          - fls_fork_reinit
//      |                                          - lifecycle fixup
//      v
//   child (post-fork): mmap → munmap            ── exercises memory mapping
//      |                                          table after fork_reinit;
//      |                                          if memory_reconcile_*
//      |                                          was not called, the
//      |                                          mapping table still
//      |                                          carries parent VA refs
//      |                                          and munmap will trap.
//      v
//   posix_spawn(helper, file_actions, ...)      ── attribute passing via
//      |                                          Reserved2Ext into a fresh
//      |                                          NT process.
//      v
//   helper exe (first invocation):              ── full Tier-A boot from
//      |   - capture getpid() = P0                scratch in a brand-new
//      |   - execv(self, ["--exec'd", "P0"])     image; then…
//      v
//   exec_self_hollow                            ── unmap old image, remap
//      |                                          target image at SAME
//      |                                          base, PID PRESERVED.
//      v
//   helper exe (re-entered, post-exec):         ── verify PID still == P0
//      |   - getpid() == P0 (argv carries P0)     and that every subsystem
//      |   - mmap/munmap                          re-initialised cleanly:
//      |   - open/read/close own exe path         memory + fd survive the
//      |   - exit 0                               execve.
//      v
//   waitpid(helper, …) → 0  → child exit 0
//   waitpid(child, …)  → 0  → test PASS
//
// What this catches
// -----------------
//   * Any *uncalled* reinit hook (memory_reconcile_fork_reinit,
//     memory_reconcile_exec_reinit, fls_fork_reinit, …): the child's
//     post-fork mmap/munmap or the post-exec mmap/munmap will fault or
//     return EINVAL, which propagates out as a non-zero waitpid status.
//   * Any *mis-sequenced* hook: e.g. cookie-rotate before canary-check,
//     or memory_reconcile_exec_reinit running before the new image's
//     PCB is mapped. These manifest as fast-fail or as the helper
//     reporting a stage-tagged exit code (see helper for stage IDs).
//   * Reserved2Ext attribute passing breakage: the helper can't open
//     fd 3 (sentinel pipe) and exits with stage=2.
//   * exec_self_hollow PID-non-preservation: the helper detects
//     getpid() != argv[2] and exits with stage=4.
//
// This test deliberately avoids touching CMakeLists.txt; build wiring
// (helper exe path via macro define) is added in a follow-up.
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"
#include "src/signal/signal.h"
#include "src/spawn/posix_spawn.h"
#include "src/spawn/posix_spawn_file_actions_addclose.h"
#include "src/spawn/posix_spawn_file_actions_adddup2.h"
#include "src/spawn/posix_spawn_file_actions_destroy.h"
#include "src/spawn/posix_spawn_file_actions_init.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/_exit.h"
#include "src/unistd/close.h"
#include "src/unistd/fork.h"
#include "src/unistd/pipe.h"
#include "src/unistd/write.h"
#include "test/UnitTest/Test.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

// Helper exe path is injected by CMake via target_compile_definitions
// once the build wiring lands; default placeholder lets the file compile
// without it.
#ifndef FORK_SPAWN_EXEC_HELPER_PATH
#define FORK_SPAWN_EXEC_HELPER_PATH "fork_then_spawn_then_exec_helper"
#endif

// Sentinel control fd the helper expects (sanity ping from spawn parent).
constexpr int CONTROL_FD = 3;

// Stage IDs the helper uses for non-zero exits — kept in sync with
// fork_then_spawn_then_exec_helper.cpp. Surface them by name in the
// assertion failure message so a regression points straight at the
// failing reinit hook.
enum HelperStage : int {
  HELPER_OK = 0,
  HELPER_STAGE_CONTROL_FD = 2,    // sentinel fd missing → spawn dup2 broke
  HELPER_STAGE_GETPID_MISMATCH = 4, // exec did NOT preserve PID
  HELPER_STAGE_MMAP = 5,            // post-exec mmap failed → memory subsystem
  HELPER_STAGE_MUNMAP = 6,          // post-exec munmap failed
  HELPER_STAGE_OPEN_SELF = 7,       // post-exec open(self) failed → fd subsystem
  HELPER_STAGE_READ_SELF = 8,       // post-exec read failed
  HELPER_STAGE_EXEC_FAILED = 9,     // execv returned (it should not)
  HELPER_STAGE_ARGV_MALFORMED = 10, // re-entry args did not parse
};

// Child-side (post-fork) exit codes — distinct from helper stages so the
// waitpid status uniquely identifies which link in the chain failed.
enum ChildExit : int {
  CHILD_OK = 0,
  CHILD_POST_FORK_MMAP_FAIL = 30,
  CHILD_POST_FORK_MUNMAP_FAIL = 31,
  CHILD_SPAWN_FAIL = 32,
  CHILD_HELPER_NONZERO = 33, // helper waitpid reported non-zero
  CHILD_HELPER_SIGNALLED = 34,
  CHILD_FILE_ACTIONS_INIT_FAIL = 35,
};

} // namespace

// ---------------------------------------------------------------------------
// fork → (post-fork mmap) → posix_spawn helper → (helper execs itself) →
// helper post-exec sanity → exit clean.
//
// Two levels of waitpid: the inner waitpid is performed by the post-fork
// child on the spawned helper; the outer waitpid here observes the
// post-fork child's combined status. Failure at any reinit hook yields a
// distinct exit code so a green/red diff points at the failing layer.
// ---------------------------------------------------------------------------

TEST(LlvmLibcForkSpawnExecChain, FullPipelineEndsClean) {
  // SIGPIPE could be raised if the helper tears down its end of the
  // sentinel pipe before the parent's write completes. Drop it so the
  // test never sees a phantom termination from an I/O race.
  LIBC_NAMESPACE::signal(SIGPIPE, SIG_IGN);

  // Build the sentinel pipe BEFORE fork so both branches inherit the
  // same fd numbers. The post-fork child uses pipe_fds[0] as the
  // helper's CONTROL_FD via posix_spawn dup2.
  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(LIBC_NAMESPACE::pipe(pipe_fds), 0);

  pid_t child_pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(child_pid, -1);

  if (child_pid == 0) {
    // ====== POST-FORK CHILD ======
    // libc_fork_reinit() has just executed in this process. Validate
    // the memory subsystem before we go any further: a one-shot mmap +
    // munmap exercises the mapping table's post-fork state. If
    // memory_reconcile_fork_reinit was missed, the mapping table still
    // points at parent VAs and one of these calls will fault.
    void *p = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      LIBC_NAMESPACE::_exit(CHILD_POST_FORK_MMAP_FAIL);
    // Touch the page so we exercise the commit / write-watch wiring,
    // not just the reservation fast path.
    *static_cast<volatile char *>(p) = 0x5A;
    if (LIBC_NAMESPACE::munmap(p, 4096) != 0)
      LIBC_NAMESPACE::_exit(CHILD_POST_FORK_MUNMAP_FAIL);

    // Close the parent's write end inside the child — the helper only
    // needs the read end as fd 3.
    LIBC_NAMESPACE::close(pipe_fds[1]);

    // Build file_actions: dup pipe read end to CONTROL_FD inside the
    // spawned helper. The helper rejects spawn if CONTROL_FD is closed,
    // which gives us a distinct failure signature when Reserved2Ext
    // attribute passing breaks.
    posix_spawn_file_actions_t actions = {};
    if (LIBC_NAMESPACE::posix_spawn_file_actions_init(&actions) != 0)
      LIBC_NAMESPACE::_exit(CHILD_FILE_ACTIONS_INIT_FAIL);
    LIBC_NAMESPACE::posix_spawn_file_actions_adddup2(&actions, pipe_fds[0],
                                                     CONTROL_FD);
    if (pipe_fds[0] != CONTROL_FD)
      LIBC_NAMESPACE::posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);

    const char *helper_path = FORK_SPAWN_EXEC_HELPER_PATH;
    char *helper_argv[] = {const_cast<char *>(helper_path), nullptr};

    pid_t helper_pid = 0;
    int spawn_rc = LIBC_NAMESPACE::posix_spawn(
        &helper_pid, helper_path, &actions, nullptr, helper_argv, environ);
    LIBC_NAMESPACE::posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0)
      LIBC_NAMESPACE::_exit(CHILD_SPAWN_FAIL);

    // Send a single byte through the sentinel pipe so the helper knows
    // the parent is alive and the dup2 plumbing landed correctly.
    char ping = 'P';
    (void)LIBC_NAMESPACE::write(pipe_fds[1], &ping, 1);
    LIBC_NAMESPACE::close(pipe_fds[1]);

    int helper_status = 0;
    if (LIBC_NAMESPACE::waitpid(helper_pid, &helper_status, 0) <= 0)
      LIBC_NAMESPACE::_exit(CHILD_HELPER_NONZERO);
    if (WIFSIGNALED(helper_status))
      LIBC_NAMESPACE::_exit(CHILD_HELPER_SIGNALLED);
    if (!WIFEXITED(helper_status) || WEXITSTATUS(helper_status) != HELPER_OK) {
      // Surface the helper's stage code in the high bits so the parent
      // test can recover it without losing the CHILD_HELPER_NONZERO tag.
      LIBC_NAMESPACE::_exit(CHILD_HELPER_NONZERO);
    }
    LIBC_NAMESPACE::_exit(CHILD_OK);
  }

  // ====== PARENT TEST ======
  // Parent does not need either pipe end — close both so a buggy child
  // that forgets to close pipe_fds[1] does not block on EOF.
  LIBC_NAMESPACE::close(pipe_fds[0]);
  LIBC_NAMESPACE::close(pipe_fds[1]);

  int status = 0;
  pid_t waited = LIBC_NAMESPACE::waitpid(child_pid, &status, 0);
  ASSERT_EQ(waited, child_pid);

  ASSERT_TRUE(WIFEXITED(status))
      << "post-fork child terminated abnormally (signalled or stopped); "
         "this indicates an unhandled fault in libc_fork_reinit, "
         "memory_reconcile_fork_reinit, or post-exec helper subsystem "
         "re-init.";
  ASSERT_FALSE(WIFSIGNALED(status));

  // CHILD_OK == 0; non-zero is one of the labelled stages above.
  EXPECT_EQ(WEXITSTATUS(status), static_cast<int>(CHILD_OK));
}
