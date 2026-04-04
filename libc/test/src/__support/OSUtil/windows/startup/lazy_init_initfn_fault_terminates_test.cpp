//===-- LazyInit InitFn fault → process termination death tests ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pin the "InitFn fault → NtTerminateProcess" contract for LazyInit<>.
//
// LazyInit<&fn>::ensure_slow() runs `fn` under a windows::FaultGuard with the
// FAULT_GUARD_DTOR mask (every hardware fault except stack overflow / debug
// traps). The historical alternative — quietly catching the fault and treating
// it as init success, leaving state_ at kReady with subsystem state never
// constructed — is the regression this test guards against. The current
// semantics are explicit in lazy_init.h's ensure_slow():
//
//     if (windows::fault_guard_enter(&g, windows::FAULT_GUARD_DTOR)) {
//       // InitFn faulted — treat as init failure.
//       ::NtTerminateProcess(NtCurrentProcess(),
//                            static_cast<NTSTATUS>(kInitFailedStatus));
//       __builtin_unreachable();
//     }
//
// where kInitFailedStatus = 0xC0000142u (STATUS_DLL_INIT_FAILED).
//
// EXPECT_DEATH is a no-op on Windows in upstream's harness, so the death
// portion of this test forks a child that triggers the fault path; the parent
// waitpid()s and asserts the encoded exit status pins the kInitFailedStatus
// path. See child_table.cpp::encode_wait_status / ntstatus_to_signal:
//
//   - 0xC0000142 is NOT in ntstatus_to_signal's table (it's a structured
//     status, not a hardware exception code), so exit_code_to_signal()
//     returns 0.
//   - encode_wait_status() therefore returns W_EXITCODE(0xC0000142 & 0xFF, 0)
//     = W_EXITCODE(0x42, 0) = 0x4200.
//   - WIFEXITED(0x4200) == true, WEXITSTATUS(0x4200) == 0x42.
//
// So a clean "FaultGuard caught the InitFn AV and called NtTerminateProcess
// with STATUS_DLL_INIT_FAILED" surfaces in the parent as
// WIFEXITED && WEXITSTATUS == 0x42. Anything else — WIFSIGNALED (the
// FaultGuard didn't catch and the AV bubbled to the OS top-level filter,
// turning into SIGSEGV via 0xC0000005), WEXITSTATUS == HAPPY_PATH_REACHED
// (FaultGuard returned without terminating, ensure() came back normally), or
// any other status — is a contract regression.
//
// The happy-path test runs in-process: an InitFn that returns 0 cleanly must
// (a) cause ensure() to return without terminating, (b) be invoked exactly
// once across multiple ensure() calls (single-flight CAS gate).
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/lazy_init.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Sentinel surfaced if the child finishes the test body without the
// FaultGuard terminating it. WIFEXITED with this value would mean the
// fault-→-terminate contract regressed to fault-→-return.
enum : int {
  HAPPY_PATH_REACHED = 55,
  CHILD_HARNESS_ERROR = 99,
};

// Expected child WEXITSTATUS on a successful FaultGuard catch.
// 0xC0000142 (STATUS_DLL_INIT_FAILED) low byte — see header banner above.
constexpr int kExpectedExitLowByte = 0x42;

// ---------------------------------------------------------------------------
// Faulting InitFn: deref nullptr through a volatile sink so the optimiser
// cannot prove the access dead. The resulting EXCEPTION_ACCESS_VIOLATION
// (0xC0000005) is in FAULT_GUARD_DTOR's mask, so the master VEH longjmps
// back into ensure_slow, which then calls NtTerminateProcess.
// ---------------------------------------------------------------------------
[[gnu::noinline]] int faulting_init_fn() {
  volatile int *p = nullptr;
  // Force the read to be observable; some optimisers will eliminate a pure
  // load-of-volatile-nullptr. The store form is unambiguously a side effect.
  *p = 0xDEAD;
  // Should never execute — the store above raises EXCEPTION_ACCESS_VIOLATION.
  return 0;
}

LIBC_NAMESPACE::internal::LazyInit<&faulting_init_fn> g_faulting_lazy;

// ---------------------------------------------------------------------------
// Happy-path InitFn: returns 0 immediately; we count invocations to confirm
// the CAS single-flight gate fires exactly once.
// ---------------------------------------------------------------------------
LIBC_NAMESPACE::cpp::Atomic<int> g_happy_invocations{0};

[[gnu::noinline]] int happy_init_fn() {
  g_happy_invocations.fetch_add(1, LIBC_NAMESPACE::cpp::MemoryOrder::SEQ_CST);
  return 0;
}

LIBC_NAMESPACE::internal::LazyInit<&happy_init_fn> g_happy_lazy;

} // namespace

// ---------------------------------------------------------------------------
// Death test: a faulting InitFn must terminate the process with
// STATUS_DLL_INIT_FAILED. The fault must NOT be silently swallowed.
// ---------------------------------------------------------------------------

TEST(LlvmLibcLazyInitFaultDeath, FaultingInitFnTerminatesWithDllInitFailed) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Child: trigger the fault path. ensure_slow's CAS winner branch will
    // run faulting_init_fn under the FaultGuard, the AV will be caught, and
    // NtTerminateProcess(0xC0000142) will fire from inside ensure_slow.
    g_faulting_lazy.ensure();

    // If we are still alive here, ensure() returned after a fault — that is
    // exactly the regression this test exists to catch. Surface a distinct
    // exit code so the assertion below pins it (and not e.g. a generic 0).
    ::NtTerminateProcess(NtCurrentProcess(), HAPPY_PATH_REACHED);
  }

  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);

  // Contract: child died via NtTerminateProcess(self, STATUS_DLL_INIT_FAILED).
  // STATUS_DLL_INIT_FAILED is not in ntstatus_to_signal's hardware-fault
  // table, so encode_wait_status takes the W_EXITCODE branch with the low
  // byte of the NTSTATUS — i.e. WIFEXITED is true and WEXITSTATUS == 0x42.
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_FALSE(WIFSIGNALED(status));
  EXPECT_EQ(WEXITSTATUS(status), kExpectedExitLowByte);

  // Belt-and-braces: explicitly reject the regression sentinels. If the
  // ensure() call returned (HAPPY_PATH_REACHED) or the AV escaped the
  // FaultGuard entirely (WIFSIGNALED with SIGSEGV), this catches it with a
  // clearer failure message than the generic byte comparison above.
  EXPECT_NE(WEXITSTATUS(status), static_cast<int>(HAPPY_PATH_REACHED));
  EXPECT_NE(WEXITSTATUS(status), static_cast<int>(CHILD_HARNESS_ERROR));
}

// ---------------------------------------------------------------------------
// Happy path: a clean InitFn must let ensure() return normally and must be
// invoked exactly once across repeat ensure() calls (single-flight gate).
// ---------------------------------------------------------------------------

TEST(LlvmLibcLazyInitFaultDeath, NonFaultingInitFnReturnsAndIsSingleFlight) {
  // Pre-condition: nothing has touched g_happy_lazy yet.
  ASSERT_EQ(g_happy_invocations.load(LIBC_NAMESPACE::cpp::MemoryOrder::SEQ_CST),
            0);
  ASSERT_FALSE(g_happy_lazy.was_initialized());

  // First ensure(): InitFn runs, returns 0, state_ becomes kReady.
  g_happy_lazy.ensure();
  EXPECT_EQ(g_happy_invocations.load(LIBC_NAMESPACE::cpp::MemoryOrder::SEQ_CST),
            1);
  EXPECT_TRUE(g_happy_lazy.was_initialized());

  // Second ensure(): fast-path acquire-load sees kReady and returns without
  // re-entering ensure_slow. Counter must stay at 1.
  g_happy_lazy.ensure();
  EXPECT_EQ(g_happy_invocations.load(LIBC_NAMESPACE::cpp::MemoryOrder::SEQ_CST),
            1);

  // Third ensure(), same expectation — pin "single-flight" beyond the
  // first/second pair so a regression that re-runs every Nth call is also
  // visible.
  g_happy_lazy.ensure();
  EXPECT_EQ(g_happy_invocations.load(LIBC_NAMESPACE::cpp::MemoryOrder::SEQ_CST),
            1);
  EXPECT_TRUE(g_happy_lazy.was_initialized());
}
