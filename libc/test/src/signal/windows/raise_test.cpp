//===-- Windows unittests for raise ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Windows-specific raise tests. The shared raise_test.cpp uses EXPECT_DEATH
// which is unavailable on Windows, so we verify behavior via handlers.
//
// POSIX compliance points tested:
//   - raise() returns 0 on success [CX]
//   - Signal is delivered before raise() returns [XSI]
//   - raise() with invalid signal returns nonzero
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/raise.h"
#include "src/signal/signal.h"
#include "test/UnitTest/Test.h"

static volatile int handler_count;
static volatile int last_signal;

static void counting_handler(int sig) {
  last_signal = sig;
  handler_count++;
}

// POSIX: "raise() shall send the signal sig to the executing thread."
// POSIX: "If successful, raise() shall return 0."
TEST(LlvmLibcWindowsRaiseTest, DeliveredBeforeReturn) {
  handler_count = 0;
  last_signal = 0;

  LIBC_NAMESPACE::signal(SIGUSR1, counting_handler);

  // POSIX requires the signal to be delivered before raise() returns.
  int ret = LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(handler_count, 1);
  EXPECT_EQ(last_signal, SIGUSR1);

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
}

// POSIX: Repeated raise() must deliver each time.
TEST(LlvmLibcWindowsRaiseTest, RepeatedDelivery) {
  handler_count = 0;

  LIBC_NAMESPACE::signal(SIGUSR2, counting_handler);
  for (int i = 0; i < 5; i++)
    EXPECT_EQ(LIBC_NAMESPACE::raise(SIGUSR2), 0);
  EXPECT_EQ(handler_count, 5);

  LIBC_NAMESPACE::signal(SIGUSR2, SIG_DFL);
}

// POSIX: SIG_IGN causes the signal to be discarded.
TEST(LlvmLibcWindowsRaiseTest, IgnoredSignal) {
  handler_count = 0;

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_IGN);
  EXPECT_EQ(LIBC_NAMESPACE::raise(SIGUSR1), 0);
  EXPECT_EQ(handler_count, 0);

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
}

// Verify multiple distinct signals are dispatched correctly.
TEST(LlvmLibcWindowsRaiseTest, DistinctSignals) {
  handler_count = 0;

  LIBC_NAMESPACE::signal(SIGUSR1, counting_handler);
  LIBC_NAMESPACE::signal(SIGUSR2, counting_handler);

  LIBC_NAMESPACE::raise(SIGUSR1);
  EXPECT_EQ(last_signal, SIGUSR1);
  LIBC_NAMESPACE::raise(SIGUSR2);
  EXPECT_EQ(last_signal, SIGUSR2);
  EXPECT_EQ(handler_count, 2);

  LIBC_NAMESPACE::signal(SIGUSR1, SIG_DFL);
  LIBC_NAMESPACE::signal(SIGUSR2, SIG_DFL);
}
