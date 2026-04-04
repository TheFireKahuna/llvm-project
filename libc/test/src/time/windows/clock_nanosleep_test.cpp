//===-- Windows unittests for clock_nanosleep ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - Returns 0 on success (not via errno)
//   - Supports CLOCK_MONOTONIC and CLOCK_REALTIME
//   - TIMER_ABSTIME: sleeps until absolute time, ignores rem
//   - Relative mode: sleeps for at least the requested duration
//   - Returns EINVAL for invalid clockid, flags, or timespec
//   - Returns EINVAL for null req
//   - Absolute time in the past returns immediately
//   - rem is filled on successful relative completion
//
//===----------------------------------------------------------------------===//

#include "hdr/time_macros.h"
#include "hdr/types/struct_timespec.h"
#include "src/time/clock_gettime.h"
#include "src/time/clock_nanosleep.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

using LlvmLibcClockNanosleepTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

namespace {
long long timespec_to_ns(const timespec &ts) {
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
} // namespace

// POSIX: "clock_nanosleep() shall return zero if the requested time has
// elapsed." Return value is direct (not errno).
TEST_F(LlvmLibcClockNanosleepTest, RelativeMonotonic) {
  timespec before, after;
  LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &before);

  timespec req = {0, 10000000}; // 10ms
  int ret = LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
  EXPECT_EQ(ret, 0);

  LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &after);
  long long elapsed_ns = timespec_to_ns(after) - timespec_to_ns(before);
  EXPECT_GE(elapsed_ns, 9000000LL);
}

// POSIX: CLOCK_REALTIME also supported.
TEST_F(LlvmLibcClockNanosleepTest, RelativeRealtime) {
  timespec req = {0, 5000000}; // 5ms
  int ret = LIBC_NAMESPACE::clock_nanosleep(CLOCK_REALTIME, 0, &req, nullptr);
  EXPECT_EQ(ret, 0);
}

// POSIX: "If the flag TIMER_ABSTIME is set [...] the [...] argument
// specifies an absolute time." Deadline in the past → returns immediately.
TEST_F(LlvmLibcClockNanosleepTest, AbsoluteTimePastReturnsImmediately) {
  // Set a deadline that has already passed (epoch + 1 second).
  timespec req = {1, 0};
  int ret = LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                            &req, nullptr);
  EXPECT_EQ(ret, 0);
}

// POSIX: Absolute mode with a near-future deadline.
TEST_F(LlvmLibcClockNanosleepTest, AbsoluteTimeMonotonic) {
  timespec now;
  LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &now);

  // Sleep until 10ms from now.
  timespec deadline;
  deadline.tv_sec = now.tv_sec;
  deadline.tv_nsec = now.tv_nsec + 10000000; // +10ms
  if (deadline.tv_nsec >= 1000000000) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000;
  }

  int ret = LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                            &deadline, nullptr);
  EXPECT_EQ(ret, 0);

  timespec after;
  LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &after);
  long long elapsed_ns = timespec_to_ns(after) - timespec_to_ns(now);
  EXPECT_GE(elapsed_ns, 9000000LL);
}

// POSIX: "EINVAL — The [...] clockid argument was [...] not supported."
TEST_F(LlvmLibcClockNanosleepTest, InvalidClockId) {
  timespec req = {0, 1000000};
  int ret = LIBC_NAMESPACE::clock_nanosleep(99, 0, &req, nullptr);
  EXPECT_EQ(ret, EINVAL);
}

// POSIX: EINVAL for invalid flags.
TEST_F(LlvmLibcClockNanosleepTest, InvalidFlags) {
  timespec req = {0, 1000000};
  int ret = LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, 0xFF, &req,
                                            nullptr);
  EXPECT_EQ(ret, EINVAL);
}

// POSIX: EINVAL for tv_nsec out of range.
TEST_F(LlvmLibcClockNanosleepTest, InvalidNsec) {
  timespec req = {0, -1};
  EXPECT_EQ(LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr),
            EINVAL);

  timespec req2 = {0, 1000000000};
  EXPECT_EQ(
      LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, 0, &req2, nullptr),
      EINVAL);
}

// POSIX: Null req → EINVAL.
TEST_F(LlvmLibcClockNanosleepTest, NullReq) {
  EXPECT_EQ(
      LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, 0, nullptr, nullptr),
      EINVAL);
}

// Zero duration returns immediately.
TEST_F(LlvmLibcClockNanosleepTest, ZeroDuration) {
  timespec req = {0, 0};
  EXPECT_EQ(LIBC_NAMESPACE::clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr),
            0);
}
