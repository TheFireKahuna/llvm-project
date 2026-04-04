//===-- $P4 fini intra-phase no-futex invariant death test ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// libc_fini_registry.h documents two hard invariants that every fini in
// `.libcfin$P*` must obey:
//
//   - No slab allocation (posix_alloc / SlabPool teardown happens at $P4 /
//     $P2; calling malloc or free from a fini AFTER its provider has run
//     walks destructed pool memory).
//   - No futex / WaitSlot acquisition. wait_slot itself is destroyed in
//     `$P4` alongside ofd_pool, file_pool, mapping_table, thread_storage,
//     named_semaphore. Within a phase bucket, intra-phase ordering is
//     linker-determined and therefore undefined by contract — so a fini
//     in $P4 that calls Futex::wait may run before OR after wait_slot's
//     own fini. If it runs after, `wait_slot::get_slot_index()` derefs the
//     freed pool (`pool = nullptr` post-fini) and the process AVs.
//
// This test pins down the second invariant with a death proof: we register
// a probe fini into `$P4` that, when armed, deliberately tears wait_slot
// down and then calls `Futex::wait()`. Run inside a forked child via
// `internal::run_all_finis()`, the access-after-free is forced to surface
// as an AV — i.e. the documented bug shape from the §4 retrospective.
//
// Why this is the right shape (vs. a passive ordering probe):
//
//   Linker merge order within `$P4` is non-deterministic but stable for a
//   given build. A passive "record-the-order" probe could pass on a build
//   where wait_slot happens to land last and never demonstrate the danger.
//   The death test, by simulating the worst-case ordering inside one
//   probe entry, proves the failure mode is real and would fire on any
//   build whose link order placed our probe after wait_slot's fini. The
//   invariant the header documents is "no fini may take a futex during
//   $P4 teardown" — the test enforces that exactly: any future change
//   that makes `Futex::wait()` survive a torn-down wait_slot would let
//   the child reach the post-wait sentinel exit, which the parent treats
//   as a regression.
//
// Why fork:
//
//   `run_all_finis()` is destructive — it walks the registry once and
//   tears down every libc subsystem in the test process. Running it
//   in-process would brick the rest of the test binary and any later
//   tests in the same image. The fork+waitpid pattern (mirroring
//   slab_pool_death_test.cpp / pcb_zone0_seal_av_test.cpp) confines the
//   damage to the child: parent observes the child's WIFSIGNALED status
//   and exits cleanly through its own DLL detach, which (importantly)
//   does NOT arm the probe — the trigger flag is set only by the child
//   after fork(), so the parent's normal `__libc_dll_fini()` walk runs
//   the probe as a no-op.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/wait_slot.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Sentinel exit codes. Anything reported via NtTerminateProcess from the
// child means the bad-fini path COMPLETED without crashing — i.e. the
// no-futex-in-$P4-fini invariant has regressed and the test must fail.
enum : int {
  REACHED_END_AFTER_WAIT = 71, // Futex::wait returned (should have AV'd)
  REACHED_END_AFTER_FINIS = 72, // run_all_finis returned (probe never ran)
  FORK_FAILED = 73,
};

// Trigger word: the probe checks this on entry and only performs the
// dangerous operations when it is non-zero. It MUST start at zero so the
// probe is a no-op during the test process's own DLL detach (which runs
// every registered fini, including this one). The child sets the flag
// after fork() before invoking run_all_finis(); parent never touches it.
//
// Atomic for visibility against any post-fork compiler reordering inside
// the child; cross-process visibility is irrelevant (parent doesn't
// share this byte after the CoW snapshot).
LIBC_NAMESPACE::cpp::Atomic<int> g_probe_armed{0};

// The probe fini. Registered into `$P4` via LIBC_REGISTER_FINI below.
//
// On a normal (parent) `__libc_dll_fini()` walk: g_probe_armed == 0, the
// function returns immediately — zero observable side effects, no
// disturbance to the rest of the registry walk.
//
// On the child's armed walk: we deterministically reproduce the
// "fini-after-wait_slot" failure mode by tearing wait_slot down ourselves
// and then calling `Futex::wait()`. This bypasses the linker's
// non-deterministic intra-$P4 ordering and forces the AV that the
// invariant exists to prevent. Without the invariant, the wait would
// silently succeed (or block forever); with the invariant correctly
// documented and the underlying state-machine matching the documentation,
// the wait MUST fault.
[[gnu::noinline]] void probe_fini() {
  if (g_probe_armed.load(LIBC_NAMESPACE::cpp::MemoryOrder::ACQUIRE) == 0)
    return;

  // Simulate the worst-case linker ordering: wait_slot's own fini has
  // already executed within this same $P4 bucket. wait_slot::fini sets
  // `pool = nullptr` and frees its TLS index; any subsequent
  // `get_slot_index()` derefs the null pool pointer.
  LIBC_NAMESPACE::wait_slot::fini();

  // Construct a local Futex with value 0 and call wait(0). Under the
  // invariant we are testing for, this must AV inside the wait_slot
  // pool deref. If it ever returns, the invariant has been silently
  // weakened and this test fails via the parent's WIFSIGNALED check.
  LIBC_NAMESPACE::Futex f{0};
  (void)f.wait(/*expected=*/0); // must AV — wait_slot pool is gone

  // Belt-and-braces: if the wait somehow returns (e.g. a fast-path
  // value-mismatch shortcut), terminate the child with a distinct
  // sentinel so the parent reports the regression precisely instead of
  // letting later finis in the walk mask the problem.
  ::NtTerminateProcess(NtCurrentProcess(), REACHED_END_AFTER_WAIT);
}

} // namespace

// File-scope registration. `tag` must be unique across the link; the
// `intra_p4_no_futex_probe` name is unlikely to collide with any
// production registration (those use subsystem names).
LIBC_REGISTER_FINI(4, intra_p4_no_futex_probe, &::probe_fini)

// ---------------------------------------------------------------------------
// $P4 intra-phase invariant: a fini that takes a Futex after wait_slot has
// torn down MUST AV. Death = invariant holds; clean exit = regression.
// ---------------------------------------------------------------------------

TEST(LlvmLibcFiniIntraP4WaitSlotVsMappingTableDeath,
     FiniThatAcquiresFutexAfterWaitSlotTeardownAVs) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Arm the probe in the child only. The CoW page diverges from the
    // parent's view, so the parent's later DLL detach still observes
    // g_probe_armed == 0 and skips the dangerous path.
    g_probe_armed.store(1, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);

    // Walk every registered fini in reverse phase order. The reverse
    // walk hits $P9..$P5 first (those subsystems' state in the CHILD is
    // either uninitialised or owned by the child alone — fork resets
    // everything that's process-bound), then $P4. Within $P4, the
    // probe runs at some linker-determined point; whether wait_slot's
    // own fini has already run by then is irrelevant — the probe forces
    // the post-wait_slot state itself before calling Futex::wait.
    LIBC_NAMESPACE::internal::run_all_finis();

    // Reaching here means run_all_finis returned without the probe
    // taking down the child — i.e. either the probe was never reached
    // (registry walker regression) or Futex::wait silently survived a
    // null wait_slot pool (invariant regression). Either way: failure.
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END_AFTER_FINIS);
  }

  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);

  // Required: the child died by signal (SIGSEGV from the AV against the
  // freed wait_slot pool, or SIGTRAP / SIGABRT if a hardening assert
  // fires earlier). Any normal exit means the no-futex-in-fini
  // invariant has been weakened and downstream subsystems are once
  // again at risk of dereferencing a destructed Treiber stack during
  // DLL detach.
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
}
