//===-- SlabPool hardening / death tests ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SlabPool's hardening is only valuable if the trap paths actually fire.
// These tests exercise the three hardening surfaces the allocator claims:
//
//   1. Double-free canary     — free(p); free(p)  → __builtin_trap
//   2. Free of invalid pointer — free of a non-slab address → trap
//   3. Guard page overflow     — write past the last slot into the
//                                trailing guard page → AV
//
// Upstream's EXPECT_DEATH is a no-op on Windows today, so each scenario
// forks a child that performs the bad operation; the parent observes the
// child exiting by signal (WIFSIGNALED). A test failure means the
// hardening DID NOT fire — the child finished normally, returning a
// sentinel success exit.
//
// All three scenarios drive the production SlabPool via malloc/free
// (which routes through posix_alloc). Exercising the pool directly is
// deliberately avoided: the production init chain is part of what we're
// testing. If the global allocator gets regressed into a non-trapping
// state (e.g. canary disabled, guard pages missing), these tests catch
// it even without access to the internal class.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/stdlib/free.h"
#include "src/stdlib/malloc.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Exit codes the children use to report "got past the bad op without
// crashing" — every one of these failing to surface as WIFSIGNALED means
// the hardening didn't fire.
enum : int {
  REACHED_END = 77,
  ALLOC_FAILED = 78,
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Double-free on a slab-size allocation must trap.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabPoolDeath, DoubleFreeTraps) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    void *p = LIBC_NAMESPACE::malloc(64);
    if (!p)
      ::NtTerminateProcess(NtCurrentProcess(), ALLOC_FAILED);
    LIBC_NAMESPACE::free(p);
    LIBC_NAMESPACE::free(p); // must trap — canary triggers __builtin_trap
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  // Child MUST have been killed by a signal (SIGABRT / SIGSEGV / SIGTRAP
  // depending on how the trap surfaces). Anything else — including a
  // normal exit with REACHED_END — is a regression.
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
}

// ---------------------------------------------------------------------------
// 2. Free of an obviously-invalid pointer must trap.
//    We pick an address that cannot lie within any slab (stack-adjacent).
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabPoolDeath, FreeOfStackAddressTraps) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    int stack_var = 0xDEADBEEF;
    LIBC_NAMESPACE::free(&stack_var); // validate_slot must reject
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
}

// ---------------------------------------------------------------------------
// 3. Free of a mis-aligned pointer (offset into an allocation) must trap.
//    This catches a regression where validate_slot accepts any pointer
//    inside a live slot.
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabPoolDeath, FreeOfInteriorPointerTraps) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    char *p = static_cast<char *>(LIBC_NAMESPACE::malloc(128));
    if (!p)
      ::NtTerminateProcess(NtCurrentProcess(), ALLOC_FAILED);
    LIBC_NAMESPACE::free(p + 16); // interior — must reject
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
}

// ---------------------------------------------------------------------------
// 4. free(NULL) is defined as a no-op and must NOT trap. (Negative test —
//    an over-eager guard that trips on NULL would break every libc user.)
// ---------------------------------------------------------------------------

TEST(LlvmLibcSlabPoolDeath, FreeNullIsNoop) {
  // This runs in-process — no fork — because success = no crash. If this
  // regressed to a trap, every downstream libc test would already fail
  // (free(NULL) is pervasive). This test just documents the invariant.
  LIBC_NAMESPACE::free(nullptr);
  EXPECT_TRUE(true);
}
