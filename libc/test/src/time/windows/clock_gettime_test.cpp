//===-- Windows unittests for clock_gettime --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/clock_gettime.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"
#include "hdr/types/struct_timespec.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcClockGettimeTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// CLOCK_REALTIME returns a plausible epoch timestamp (after 2020-01-01).
TEST_F(LlvmLibcClockGettimeTest, RealtimePlausible) {
  struct timespec ts;
  ASSERT_THAT(LIBC_NAMESPACE::clock_gettime(CLOCK_REALTIME, &ts), Succeeds());
  // 2020-01-01 00:00:00 UTC = 1577836800
  EXPECT_GT(ts.tv_sec, static_cast<time_t>(1577836800));
  EXPECT_GE(ts.tv_nsec, static_cast<long>(0));
  EXPECT_LT(ts.tv_nsec, static_cast<long>(1000000000));
}

// CLOCK_MONOTONIC is non-decreasing across two calls.
TEST_F(LlvmLibcClockGettimeTest, MonotonicNonDecreasing) {
  struct timespec t1, t2;
  ASSERT_THAT(LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &t1), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::clock_gettime(CLOCK_MONOTONIC, &t2), Succeeds());
  // t2 >= t1
  bool non_decreasing = (t2.tv_sec > t1.tv_sec) ||
                        (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec);
  EXPECT_TRUE(non_decreasing);
}

// Invalid clock ID returns -1 and sets EINVAL.
TEST_F(LlvmLibcClockGettimeTest, InvalidClockId) {
  struct timespec ts;
  EXPECT_THAT(LIBC_NAMESPACE::clock_gettime(-1, &ts), Fails(EINVAL));
}
