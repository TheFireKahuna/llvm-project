//===-- libc_fork_reinit() pre-rotate canary validation death test --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PCB Zone 0b carries three correlated fields: security_cookie,
// security_cookie_complement (== ~security_cookie), and zone_canary
// (== security_cookie ^ PCB_CANARY_MAGIC). The triple is sealed
// PAGE_READONLY for process lifetime except inside the narrow unseal
// windows owned by libc_fork_reinit() and the veh_core fini path.
//
// libc_fork_reinit() runs in the child immediately after fork. Before
// rotating the cookie/complement/canary triple to fresh CSPRNG-derived
// values, it MUST validate the inherited (parent) values:
//
//   pcb_unseal_readonly_b();
//   if (!pcb_check_canary())
//     NtTerminateProcess(self, 127);     // <-- pre-rotate validation
//   rotate_pcb_security_cookie();         // <-- new cookie, new canary
//   ... per-subsystem fork_reinit ...
//   if (!pcb_check_canary())
//     NtTerminateProcess(self, 127);     // <-- post-window validation
//   pcb_seal_readonly_b();
//
// The pre-rotate check is the anti-corruption / anti-exfil guarantee:
// if a parent-side attacker (linear overflow that reaches Zone 0b during
// an unseal window, or a buggy libc-internal write) corrupted any of the
// three fields, the child must abort BEFORE trusting the values to
// reseed downstream subsystem state. Without this check, the rotate
// would silently fix the canary mismatch up by overwriting all three
// fields with fresh PRNG output, masking the corruption that occurred
// in the parent.
//
// Strategy
// --------
//   1. Parent: open a Zone 0b unseal window and corrupt the canary so
//      `zone_canary != security_cookie ^ PCB_CANARY_MAGIC`. Reseal Zone 0b
//      so the child starts with the same protection state the production
//      fork path expects.
//   2. Parent: fork.
//   3. Child: fork() in the child runs fork_child() → libc_fork_reinit().
//      The pre-rotate `pcb_check_canary()` returns false → child aborts
//      with NtTerminateProcess(self, 127).
//   4. Parent: waitpid; expect WIFEXITED with exit code 127. (Termination
//      via NtTerminateProcess surfaces as a normal exit, not a signal —
//      the master VEH never sees the call.)
//   5. Parent: restore the canary to its valid value (still inside an
//      unseal/reseal pair) so subsequent tests are not poisoned.
//
// The test deliberately corrupts the canary field rather than the cookie
// or the complement: pcb_check_canary() compares the canary against
// (cookie ^ MAGIC), so corrupting EITHER side of that equality is
// sufficient to fire the pre-rotate validation. Mutating the canary
// alone leaves the cookie/complement pair self-consistent in the parent,
// which keeps the parent's own post-corruption code paths (anything that
// reads the cookie before the next reseal) safe to run between corruption
// and fork.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Sentinel: child reached the end of libc_fork_reinit without aborting.
// If this surfaces as the exit status, the pre-rotate canary check did
// NOT fire — that is the regression these tests guard against.
enum : int {
  REACHED_END = 77,
};

// Exit code libc_fork_reinit uses on canary failure (see
// libc_fork_reinit_impl.cpp: `NtTerminateProcess(NtCurrentProcess(), 127);`).
// Pinned here so a refactor that changes the abort code is caught by an
// explicit, descriptive assertion failure rather than a vague "wrong
// status" diff.
inline constexpr int CANARY_ABORT_EXIT = 127;

// RAII-style helper: open a Zone 0b unseal window for the duration of the
// scope. Both unseal and reseal go through the same code path
// libc_fork_reinit uses, so the test exercises the production protection
// state machine rather than poking PTEs directly.
struct Zone0bUnsealScope {
  bool ok;
  Zone0bUnsealScope() {
    ok = LIBC_NAMESPACE::pcb_unseal_readonly_b();
  }
  ~Zone0bUnsealScope() {
    if (ok)
      (void)LIBC_NAMESPACE::pcb_seal_readonly_b();
  }
  Zone0bUnsealScope(const Zone0bUnsealScope &) = delete;
  Zone0bUnsealScope &operator=(const Zone0bUnsealScope &) = delete;
};

// Corrupt the Zone 0b canary so pcb_check_canary() returns false. Caller
// must hold a Zone0bUnsealScope. We XOR a non-zero pattern into the
// existing canary value so the corruption is reversible by repeating the
// same XOR, which lets the parent restore a valid state cleanly without
// having to recompute (cookie ^ PCB_CANARY_MAGIC) from scratch.
inline constexpr uintptr_t kCorruptionMask = 0xDEADBEEFCAFEBABEULL;

[[gnu::noinline]] void flip_canary_bits() {
  // PcbInitAccess only exposes init_canary() (recompute from cookie); to
  // inject a *bad* value we go through the friend relationship the same
  // way init_canary does — write the private field directly via a
  // local PcbInitAccess specialisation. The cleanest legal path is to
  // briefly install a bogus cookie, call init_canary(), then restore
  // the cookie — this leaves canary = bogus_cookie ^ MAGIC, which no
  // longer matches the real cookie.
  uintptr_t real_cookie = LIBC_NAMESPACE::g_pcb.zone0b.security_cookie();
  uintptr_t bogus_cookie = real_cookie ^ kCorruptionMask;
  LIBC_NAMESPACE::internal::PcbInitAccess::set_security_cookie(bogus_cookie);
  LIBC_NAMESPACE::internal::PcbInitAccess::init_canary();
  // Restore the real cookie. Canary now references the bogus cookie,
  // so pcb_check_canary() == false. Complement is untouched (still
  // ~real_cookie), which is fine — the pre-rotate check only consults
  // the canary/cookie equality, not the complement.
  LIBC_NAMESPACE::internal::PcbInitAccess::set_security_cookie(real_cookie);
}

// Restore canary to the valid value (cookie ^ MAGIC). Caller must hold
// a Zone0bUnsealScope. Idempotent.
[[gnu::noinline]] void restore_canary() {
  LIBC_NAMESPACE::internal::PcbInitAccess::init_canary();
}

} // namespace

// ---------------------------------------------------------------------------
// Pre-rotate canary validation must fire on a corrupted parent canary.
//
// If libc_fork_reinit() ever stops calling pcb_check_canary() before the
// rotate (or starts ignoring its return), the child will reach REACHED_END
// rather than exiting with CANARY_ABORT_EXIT, and this test fails.
// ---------------------------------------------------------------------------

TEST(LlvmLibcForkReinitCanary, PreRotateCheckFiresOnCorruptedCanary) {
  // Sanity: canary is valid in the parent before we touch anything. If
  // some prior test already corrupted Zone 0b without restoring it, the
  // failure should surface here with a clear message instead of being
  // attributed to this test.
  ASSERT_TRUE(LIBC_NAMESPACE::pcb_check_canary());

  // Corrupt the canary so pcb_check_canary() returns false. The child
  // will inherit this state via CoW.
  {
    Zone0bUnsealScope unseal;
    ASSERT_TRUE(unseal.ok);
    flip_canary_bits();
    ASSERT_FALSE(LIBC_NAMESPACE::pcb_check_canary());
  }

  // At this point Zone 0b is resealed PAGE_READONLY with a corrupt
  // canary. Fork — the child's libc_fork_reinit() will detect the bad
  // canary on the pre-rotate check and abort.
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Unreachable: the fork-child path enters libc_fork_reinit before
    // returning from fork(), and the pre-rotate canary check terminates
    // the process. If we surface here, the validation did not fire and
    // the child is now running on an attacker-influenced cookie/canary.
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }

  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);

  // Restore Zone 0b to a valid state in the parent so subsequent tests
  // (and any background libc activity that touches the canary, e.g. a
  // later fork in this same test binary) see a consistent triple. Do
  // this BEFORE the result assertions so a test failure does not leave
  // the process in a poisoned state for the next TEST_F.
  {
    Zone0bUnsealScope unseal;
    ASSERT_TRUE(unseal.ok);
    restore_canary();
    ASSERT_TRUE(LIBC_NAMESPACE::pcb_check_canary());
  }

  // libc_fork_reinit aborts via NtTerminateProcess(self, 127), which
  // surfaces as a normal exit with status 127 — NOT a signal. WIFEXITED
  // must be true and the exit code must match the documented constant.
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_FALSE(WIFSIGNALED(status));
  EXPECT_EQ(WEXITSTATUS(status), CANARY_ABORT_EXIT);
}

// ---------------------------------------------------------------------------
// Negative control: an uncorrupted canary lets the child complete
// libc_fork_reinit() and exit normally.
//
// Without this control, a regression that changes the abort code (or
// makes pcb_check_canary always return false) could pass the positive
// test by exiting with 127 unconditionally. Pairing the two checks
// pins the behaviour: 127 iff corrupted, anything-else (specifically
// our REACHED_END sentinel) iff clean.
// ---------------------------------------------------------------------------

TEST(LlvmLibcForkReinitCanary, CleanCanaryAllowsChildToProceed) {
  ASSERT_TRUE(LIBC_NAMESPACE::pcb_check_canary());

  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Child's libc_fork_reinit() has already run by the time fork()
    // returns 0. If we are here, pre-rotate validation accepted the
    // (clean) inherited canary and the rotate produced a fresh, valid
    // triple in the child. Verify the new canary is self-consistent
    // before exiting cleanly.
    if (!LIBC_NAMESPACE::pcb_check_canary())
      ::NtTerminateProcess(NtCurrentProcess(), CANARY_ABORT_EXIT);
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }

  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);

  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_FALSE(WIFSIGNALED(status));
  EXPECT_EQ(WEXITSTATUS(status), static_cast<int>(REACHED_END));
}
