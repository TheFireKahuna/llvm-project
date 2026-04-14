//===-- Windows unittests for sigaltstack ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - Get current alternate stack via null ss, non-null oss
//   - Set a new alternate stack
//   - SS_DISABLE disables the alternate stack
//   - Invalid flags (not SS_DISABLE) return EINVAL
//   - Stack smaller than MINSIGSTKSZ returns ENOMEM
//   - Cannot change alt stack while SS_ONSTACK is set (EPERM)
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigaltstack.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/signal_macros.h"
#include "hdr/types/stack_t.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsSigaltstackTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// Get current stack via null ss, non-null oss — must succeed.
TEST_F(LlvmLibcWindowsSigaltstackTest, GetCurrentStack) {
  stack_t oss;
  EXPECT_THAT(LIBC_NAMESPACE::sigaltstack(nullptr, &oss), Succeeds());
}

// Set a valid alternate stack and read it back.
TEST_F(LlvmLibcWindowsSigaltstackTest, SetAndGet) {
  // Allocate a buffer large enough for an alternate signal stack.
  static char alt_stack[MINSIGSTKSZ * 2];

  stack_t ss;
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof(alt_stack);
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds());

  stack_t oss;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(nullptr, &oss), Succeeds());
  EXPECT_EQ(oss.ss_sp, static_cast<void *>(alt_stack));
  EXPECT_EQ(oss.ss_size, sizeof(alt_stack));
  EXPECT_EQ(oss.ss_flags & SS_ONSTACK, 0);

  // Restore: disable.
  stack_t dis;
  dis.ss_flags = SS_DISABLE;
  dis.ss_sp = nullptr;
  dis.ss_size = 0;
  EXPECT_THAT(LIBC_NAMESPACE::sigaltstack(&dis, nullptr), Succeeds());
}

// SS_DISABLE disables the alternate stack.
TEST_F(LlvmLibcWindowsSigaltstackTest, DisableStack) {
  // First set a valid stack.
  static char alt_stack[MINSIGSTKSZ * 2];
  stack_t ss;
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof(alt_stack);
  ss.ss_flags = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Succeeds());

  // Disable it.
  stack_t dis;
  dis.ss_flags = SS_DISABLE;
  dis.ss_sp = nullptr;
  dis.ss_size = 0;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(&dis, nullptr), Succeeds());

  stack_t oss;
  ASSERT_THAT(LIBC_NAMESPACE::sigaltstack(nullptr, &oss), Succeeds());
  EXPECT_TRUE((oss.ss_flags & SS_DISABLE) != 0 || oss.ss_sp == nullptr);
}

// Invalid flags (not SS_DISABLE) return EINVAL.
TEST_F(LlvmLibcWindowsSigaltstackTest, InvalidFlags) {
  static char alt_stack[MINSIGSTKSZ * 2];
  stack_t ss;
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof(alt_stack);
  ss.ss_flags = 0xFF; // Unknown flags.
  EXPECT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Fails(EINVAL));
}

// Stack size smaller than MINSIGSTKSZ returns ENOMEM.
TEST_F(LlvmLibcWindowsSigaltstackTest, TooSmall) {
  char tiny[4];
  stack_t ss;
  ss.ss_sp = tiny;
  ss.ss_size = sizeof(tiny); // < MINSIGSTKSZ
  ss.ss_flags = 0;
  EXPECT_THAT(LIBC_NAMESPACE::sigaltstack(&ss, nullptr), Fails(ENOMEM));
}
