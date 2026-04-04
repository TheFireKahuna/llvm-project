//===-- Windows unittests for nanosleep ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - nanosleep() returns 0 on success
//   - Sleeps for at least the requested duration
//   - Zero duration returns immediately
//   - EINVAL for negative tv_nsec
//   - EINVAL for tv_nsec >= 1,000,000,000
//   - EINVAL for null req pointer
//   - rem is zeroed on successful completion
//
//===----------------------------------------------------------------------===//

#include "hdr/time_macros.h"
#include "hdr/types/struct_timespec.h"
#include "src/time/clock_gettime.h"
#include "src/time/nanosleep.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

using LlvmLibcNanosleepTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;

namespace {
long long timespec_to_ns(const timespec &ts) {
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
} // namespace

// POSIX: "nanosleep() shall cause the current thread to be suspended
// from execution until [...] the time interval specified by the rqtp
// argument has elapsed."
TEST_F(LlvmLibcNanosleepTest, SleepsDuration) {
  timespec before, after;
  LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &before);

  timespec req = {0, 10000000}; // 10ms
  timespec rem = {99, 99};
  EXPECT_EQ(LIBC_NAMESPACE::nanosleep(&req, &rem), 0);

  LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &after);

  long long elapsed_ns = timespec_to_ns(after) - timespec_to_ns(before);
  // Must have slept at least 10ms (allowing tiny clock skew).
  EXPECT_GE(elapsed_ns, 9000000LL);

  // POSIX: on success, rem should be zeroed (no remaining time).
  EXPECT_EQ(rem.tv_sec, static_cast<decltype(rem.tv_sec)>(0));
  EXPECT_EQ(rem.tv_nsec, static_cast<decltype(rem.tv_nsec)>(0));
}

// POSIX: Zero duration returns immediately.
TEST_F(LlvmLibcNanosleepTest, ZeroDuration) {
  timespec req = {0, 0};
  timespec rem = {99, 99};
  EXPECT_EQ(LIBC_NAMESPACE::nanosleep(&req, &rem), 0);
  EXPECT_EQ(rem.tv_sec, static_cast<decltype(rem.tv_sec)>(0));
  EXPECT_EQ(rem.tv_nsec, static_cast<decltype(rem.tv_nsec)>(0));
}

// POSIX: "If [...] tv_nsec is not in the range [0, 999999999] [...]
// nanosleep() shall return -1 with errno set to [EINVAL]."
TEST_F(LlvmLibcNanosleepTest, InvalidNsecNegative) {
  timespec req = {0, -1};
  EXPECT_THAT(LIBC_NAMESPACE::nanosleep(&req, nullptr), Fails(EINVAL));
}

TEST_F(LlvmLibcNanosleepTest, InvalidNsecTooLarge) {
  timespec req = {0, 1000000000};
  EXPECT_THAT(LIBC_NAMESPACE::nanosleep(&req, nullptr), Fails(EINVAL));
}

// POSIX: Null req pointer → EINVAL.
TEST_F(LlvmLibcNanosleepTest, NullReq) {
  EXPECT_THAT(LIBC_NAMESPACE::nanosleep(nullptr, nullptr), Fails(EINVAL));
}

// Verify rem=nullptr doesn't crash.
TEST_F(LlvmLibcNanosleepTest, NullRem) {
  timespec req = {0, 1000000}; // 1ms
  EXPECT_EQ(LIBC_NAMESPACE::nanosleep(&req, nullptr), 0);
}
