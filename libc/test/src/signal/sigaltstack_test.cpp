//===-- Unittests for sigaltstack -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "hdr/stdint_proxy.h"
#include "src/signal/raise.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigaltstack.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#if !defined(_WIN32) && !defined(_WIN32_ITANIUM)
#include "src/__support/OSUtil/syscall.h"
#include "src/signal/linux/signal_utils.h"
#include <sys/syscall.h>
#endif

constexpr int LOCAL_VAR_SIZE = 512;
constexpr int ALT_STACK_SIZE = SIGSTKSZ + LOCAL_VAR_SIZE * 2;
static uint8_t alt_stack_buf[ALT_STACK_SIZE];

using LlvmLibcSigaltstackTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// Helper: configure alt stack and a SIGUSR1 handler with given flags.
static void setup_alt_stack_handler(void (*handler_fn)(int), int sa_flags) {
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = ALT_STACK_SIZE;
  ss.ss_flags = 0;
  LIBC_NAMESPACE::sigaltstack(&ss, nullptr);

  struct sigaction action;
  LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &action);
  action.sa_handler = handler_fn;
  action.sa_flags = sa_flags;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);
}

// Helper: clean up alt stack and handler.
static void cleanup_alt_stack_handler() {
  stack_t ss;
  ss.ss_flags = SS_DISABLE;
  ss.ss_sp = nullptr;
  ss.ss_size = 0;
  LIBC_NAMESPACE::sigaltstack(&ss, nullptr);

  struct sigaction action;
  LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &action);
  action.sa_handler = SIG_DFL;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);
}

// --- API Tests ---

TEST_F(LlvmLibcSigaltstackTest, SigaltstackInvalidStack) {
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = 0;
  // SS_ONSTACK is not a valid flag to set.
  ss.ss_flags = SS_ONSTACK;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Fails(EINVAL));

  // Size below MINSIGSTKSZ.
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Fails(ENOMEM));
}

TEST_F(LlvmLibcSigaltstackTest, SigaltstackGetCurrent) {
  // Initially no alt stack — should report SS_DISABLE.
  stack_t oss;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(nullptr, &oss), Succeeds(0));
  EXPECT_EQ(oss.ss_flags & SS_DISABLE, SS_DISABLE);

  // Configure an alt stack.
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = ALT_STACK_SIZE;
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds(0));

  // Verify it was stored.
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(nullptr, &oss), Succeeds(0));
  EXPECT_EQ(oss.ss_sp, static_cast<void *>(alt_stack_buf));
  EXPECT_EQ(static_cast<int>(oss.ss_size), ALT_STACK_SIZE);
  EXPECT_EQ(oss.ss_flags & SS_DISABLE, 0);

  // Disable and verify old state returned.
  ss.ss_flags = SS_DISABLE;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, &oss), Succeeds(0));
  EXPECT_EQ(oss.ss_sp, static_cast<void *>(alt_stack_buf));

  // Current state should be disabled.
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(nullptr, &oss), Succeeds(0));
  EXPECT_EQ(oss.ss_flags & SS_DISABLE, SS_DISABLE);
}

TEST_F(LlvmLibcSigaltstackTest, SigaltstackDisable) {
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = ALT_STACK_SIZE;
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds(0));

  stack_t disable;
  disable.ss_flags = SS_DISABLE;
  disable.ss_sp = nullptr;
  disable.ss_size = 0;
  stack_t old;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&disable, &old), Succeeds(0));

  EXPECT_EQ(old.ss_sp, static_cast<void *>(alt_stack_buf));
  EXPECT_EQ(static_cast<int>(old.ss_size), ALT_STACK_SIZE);
  EXPECT_EQ(old.ss_flags & SS_DISABLE, 0);
}

TEST_F(LlvmLibcSigaltstackTest, SigaltstackMinSize) {
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = MINSIGSTKSZ;
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds(0));

  ss.ss_size = MINSIGSTKSZ - 1;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Fails(ENOMEM));

  ss.ss_flags = SS_DISABLE;
  LIBC_NAMESPACE::sigaltstack(&ss, nullptr);
}

// --- Integration Tests ---

static bool good_stack;

static void stack_check_handler(int) {
  uint8_t var[LOCAL_VAR_SIZE];
  for (int i = 0; i < LOCAL_VAR_SIZE; ++i)
    var[i] = i;
  // Verify that the local array is entirely within the alt stack buffer.
  for (int i = 0; i < LOCAL_VAR_SIZE; ++i) {
    if (!(uintptr_t(var + i) < uintptr_t(alt_stack_buf + ALT_STACK_SIZE) &&
          uintptr_t(alt_stack_buf) <= uintptr_t(var + i))) {
      good_stack = false;
      return;
    }
  }
  good_stack = true;
}

// Raise SIGUSR1 with SA_ONSTACK — handler should run on the alt stack.
TEST_F(LlvmLibcSigaltstackTest, RunOnAltStack) {
  setup_alt_stack_handler(stack_check_handler, SA_ONSTACK);

  good_stack = false;
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_TRUE(good_stack);

  cleanup_alt_stack_handler();
}

static bool not_on_alt_stack;

static void normal_stack_handler(int) {
  uint8_t var;
  // Should NOT be on the alt stack.
  not_on_alt_stack =
      uintptr_t(&var) < uintptr_t(alt_stack_buf) ||
      uintptr_t(&var) >= uintptr_t(alt_stack_buf + ALT_STACK_SIZE);
}

// Without SA_ONSTACK, handler should run on the normal stack even if
// sigaltstack is configured.
TEST_F(LlvmLibcSigaltstackTest, NoSAOnstackUsesNormalStack) {
  setup_alt_stack_handler(normal_stack_handler, 0 /* no SA_ONSTACK */);

  not_on_alt_stack = false;
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_TRUE(not_on_alt_stack);

  cleanup_alt_stack_handler();
}

static int ss_onstack_during;

static void onstack_flag_handler(int) {
  stack_t oss;
  LIBC_NAMESPACE::sigaltstack(nullptr, &oss);
  ss_onstack_during = oss.ss_flags & SS_ONSTACK;
}

// SS_ONSTACK should be set while the handler is executing on the alt stack,
// and cleared after the handler returns.
TEST_F(LlvmLibcSigaltstackTest, SSOnstackDuringHandler) {
  setup_alt_stack_handler(onstack_flag_handler, SA_ONSTACK);

  ss_onstack_during = 0;
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(ss_onstack_during, SS_ONSTACK);

  // After handler returns, SS_ONSTACK should be cleared.
  stack_t oss;
  LIBC_NAMESPACE::sigaltstack(nullptr, &oss);
  EXPECT_EQ(oss.ss_flags & SS_ONSTACK, 0);

  cleanup_alt_stack_handler();
}

static bool siginfo_on_alt_stack;

static void siginfo_handler(int signum, siginfo_t *info, void *) {
  (void)info;
  uint8_t var;
  siginfo_on_alt_stack =
      signum == SIGUSR1 &&
      uintptr_t(&var) >= uintptr_t(alt_stack_buf) &&
      uintptr_t(&var) < uintptr_t(alt_stack_buf + ALT_STACK_SIZE);
}

// SA_SIGINFO handler on alt stack.
TEST_F(LlvmLibcSigaltstackTest, SigInfoOnAltStack) {
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = ALT_STACK_SIZE;
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds(0));

  struct sigaction action;
  LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &action);
  action.sa_sigaction = siginfo_handler;
  action.sa_flags = SA_ONSTACK | SA_SIGINFO;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  siginfo_on_alt_stack = false;
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_TRUE(siginfo_on_alt_stack);

  cleanup_alt_stack_handler();
}

static int nested_handler_count;
static bool nested_on_alt_stack;

static void nested_handler(int) {
  ++nested_handler_count;
  uint8_t var;
  nested_on_alt_stack =
      uintptr_t(&var) >= uintptr_t(alt_stack_buf) &&
      uintptr_t(&var) < uintptr_t(alt_stack_buf + ALT_STACK_SIZE);

  // Raise again during handler — SS_ONSTACK prevents recursive alt stack use.
  // With SA_NODEFER the signal is not blocked, so this will deliver immediately.
  if (nested_handler_count == 1)
    LIBC_NAMESPACE::raise(SIGUSR1);
}

// Nested signal delivery: second handler call should NOT re-enter the alt stack
// (SS_ONSTACK prevents it), so it runs on the current stack (which is the alt
// stack from the first call, but not via another trampoline switch).
TEST_F(LlvmLibcSigaltstackTest, NestedSignalOnAltStack) {
  stack_t ss;
  ss.ss_sp = alt_stack_buf;
  ss.ss_size = ALT_STACK_SIZE;
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds(0));

  struct sigaction action;
  LIBC_NAMESPACE::sigaction(SIGUSR1, nullptr, &action);
  action.sa_handler = nested_handler;
  action.sa_flags = SA_ONSTACK | SA_NODEFER;
  LIBC_NAMESPACE::sigaction(SIGUSR1, &action, nullptr);

  nested_handler_count = 0;
  nested_on_alt_stack = false;
  LIBC_NAMESPACE::raise(SIGUSR1);
  // Both calls should have executed.
  EXPECT_EQ(nested_handler_count, 2);

  cleanup_alt_stack_handler();
}

// Test that delivering twice reuses the alt stack correctly (SS_ONSTACK
// cleared between calls).
TEST_F(LlvmLibcSigaltstackTest, RepeatedDelivery) {
  setup_alt_stack_handler(stack_check_handler, SA_ONSTACK);

  good_stack = false;
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_TRUE(good_stack);

  good_stack = false;
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_TRUE(good_stack);

  // SS_ONSTACK should be cleared after each delivery.
  stack_t oss;
  LIBC_NAMESPACE::sigaltstack(nullptr, &oss);
  EXPECT_EQ(oss.ss_flags & SS_ONSTACK, 0);

  cleanup_alt_stack_handler();
}
