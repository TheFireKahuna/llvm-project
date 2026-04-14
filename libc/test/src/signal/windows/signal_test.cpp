//===-- Windows unittests for signal ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - signal() returns SIG_ERR and sets errno to EINVAL for invalid signals
//   - signal() returns the previous handler on success
//   - Handler persists across deliveries (not SysV one-shot reset)
//   - SIG_IGN and SIG_DFL are accepted dispositions
//
//===----------------------------------------------------------------------===//

#include "src/signal/raise.h"
#include "src/signal/signal.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

using LlvmLibcWindowsSignalTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// POSIX: "If sig is not a valid signal [...] signal() shall return SIG_ERR
// and set errno to [EINVAL]."
TEST(LlvmLibcWindowsSignalTest, InvalidSignalNumber) {
  auto *valid = +[](int) {};
  EXPECT_THAT((void *)LIBC_NAMESPACE::signal(0, valid),
              Fails(EINVAL, (void *)SIG_ERR));
  EXPECT_THAT((void *)LIBC_NAMESPACE::signal(65, valid),
              Fails(EINVAL, (void *)SIG_ERR));
}

static volatile int signal_count;

// POSIX: Handler persists — not reset to SIG_DFL after first delivery.
// This is the BSD (modern) behavior vs. the SysV one-shot behavior.
TEST(LlvmLibcWindowsSignalTest, HandlerPersistsAcrossDeliveries) {
  signal_count = 0;

  ASSERT_NE(LIBC_NAMESPACE::signal(SIGUSR1, +[](int) { signal_count++; }),
            SIG_ERR);
  ASSERT_THAT(LIBC_NAMESPACE::raise(SIGUSR1), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::raise(SIGUSR1), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::raise(SIGUSR1), Succeeds());
  EXPECT_EQ(signal_count, 3);

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
}

// POSIX: "signal() shall return the previous signal handler."
TEST(LlvmLibcWindowsSignalTest, ReturnsPreviousHandler) {
  auto *handler_a = +[](int) {};
  auto *handler_b = +[](int) {};

  // Install handler_a, should return SIG_DFL (or whatever was set before).
  auto *prev = LIBC_NAMESPACE::signal(SIGUSR1, handler_a);
  // Now install handler_b, should return handler_a.
  prev = LIBC_NAMESPACE::signal(SIGUSR1, handler_b);
  EXPECT_EQ(prev, handler_a);
  // Restore, should return handler_b.
  prev = LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
  EXPECT_EQ(prev, handler_b);
}

// POSIX: SIG_IGN causes the signal to be discarded silently.
TEST(LlvmLibcWindowsSignalTest, IgnoreDisposition) {
  signal_count = 0;

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_IGN);
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(signal_count, 0);

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
}

// Verify handler replacement atomicity — new handler receives signals
// immediately after installation.
TEST(LlvmLibcWindowsSignalTest, HandlerReplacement) {
  signal_count = 0;

  LIBC_NAMESPACE::signal(SIGUSR1, +[](int) { signal_count += 10; });
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(signal_count, 10);

  LIBC_NAMESPACE::signal(SIGUSR1, +[](int) { signal_count += 100; });
  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(signal_count, 110);

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
}
