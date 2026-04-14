//===-- Windows unittests for sigset operations ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - sigemptyset: initializes set with all signals excluded
//   - sigfillset: initializes set with all signals included
//   - sigaddset: adds a signal to the set, returns 0
//   - sigdelset: removes a signal from the set, returns 0
//   - sigismember: returns 1 if member, 0 if not
//   - All return -1 with EINVAL for invalid signal numbers
//   - Operations are independent (add one doesn't affect others)
//
//===----------------------------------------------------------------------===//

#include "hdr/signal_macros.h"
#include "src/signal/sigaddset.h"
#include "src/signal/sigdelset.h"
#include "src/signal/sigemptyset.h"
#include "src/signal/sigfillset.h"
#include "src/signal/sigismember.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;

// POSIX: "sigemptyset() initializes the signal set pointed to by set,
// such that all signals [...] are excluded."
TEST(LlvmLibcWindowsSigsetTest, EmptySet) {
  sigset_t set;
  EXPECT_EQ(LIBC_NAMESPACE::sigemptyset(&set), 0);

  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR2), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGINT), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGTERM), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGABRT), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGSEGV), 0);
}

// POSIX: "sigfillset() initializes the signal set [...] such that all
// signals [...] are included."
TEST(LlvmLibcWindowsSigsetTest, FillSet) {
  sigset_t set;
  EXPECT_EQ(LIBC_NAMESPACE::sigfillset(&set), 0);

  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR2), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGINT), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGTERM), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGABRT), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGSEGV), 1);
}

// POSIX: sigaddset adds exactly one signal; others remain unchanged.
TEST(LlvmLibcWindowsSigsetTest, AddIsIndependent) {
  sigset_t set;
  LIBC_NAMESPACE::sigemptyset(&set);

  EXPECT_EQ(LIBC_NAMESPACE::sigaddset(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR2), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGTERM), 0);

  EXPECT_EQ(LIBC_NAMESPACE::sigaddset(&set, SIGUSR2), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR2), 1);
}

// POSIX: sigdelset removes exactly one signal; others remain unchanged.
TEST(LlvmLibcWindowsSigsetTest, DeleteIsIndependent) {
  sigset_t set;
  LIBC_NAMESPACE::sigfillset(&set);

  EXPECT_EQ(LIBC_NAMESPACE::sigdelset(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR2), 1);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGTERM), 1);
}

// POSIX: "If signo is not a valid signal number, the behavior is undefined"
// but implementations should return -1 with EINVAL.
TEST(LlvmLibcWindowsSigsetTest, InvalidSignalNumber) {
  sigset_t set;
  LIBC_NAMESPACE::sigemptyset(&set);

  EXPECT_THAT(LIBC_NAMESPACE::sigaddset(&set, 0), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::sigaddset(&set, 65), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::sigdelset(&set, -1), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::sigismember(&set, 0), Fails(EINVAL));
}

// POSIX: sigaddset on an already-present signal is idempotent.
TEST(LlvmLibcWindowsSigsetTest, AddIdempotent) {
  sigset_t set;
  LIBC_NAMESPACE::sigemptyset(&set);

  EXPECT_EQ(LIBC_NAMESPACE::sigaddset(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigaddset(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 1);
}

// POSIX: sigdelset on an already-absent signal is idempotent.
TEST(LlvmLibcWindowsSigsetTest, DeleteIdempotent) {
  sigset_t set;
  LIBC_NAMESPACE::sigemptyset(&set);

  EXPECT_EQ(LIBC_NAMESPACE::sigdelset(&set, SIGUSR1), 0);
  EXPECT_EQ(LIBC_NAMESPACE::sigismember(&set, SIGUSR1), 0);
}
