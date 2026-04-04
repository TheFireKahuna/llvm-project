//===-- Windows unittests for clock_getres --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/clock_getres.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"
#include "hdr/types/struct_timespec.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcClockGetresTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// CLOCK_REALTIME resolution is non-zero and sub-second.
TEST_F(LlvmLibcClockGetresTest, RealtimeResolution) {
  struct timespec res;
  ASSERT_THAT(LIBC_NAMESPACE::clock_getres(CLOCK_REALTIME, &res), Succeeds());
  bool nonzero = res.tv_sec > 0 || res.tv_nsec > 0;
  EXPECT_TRUE(nonzero);
  // Resolution should be at most 1 second.
  EXPECT_EQ(res.tv_sec, static_cast<time_t>(0));
  EXPECT_GT(res.tv_nsec, static_cast<long>(0));
  EXPECT_LT(res.tv_nsec, static_cast<long>(1000000000));
}

// CLOCK_MONOTONIC resolution is non-zero and sub-second.
TEST_F(LlvmLibcClockGetresTest, MonotonicResolution) {
  struct timespec res;
  ASSERT_THAT(LIBC_NAMESPACE::clock_getres(CLOCK_MONOTONIC, &res), Succeeds());
  bool nonzero = res.tv_sec > 0 || res.tv_nsec > 0;
  EXPECT_TRUE(nonzero);
  EXPECT_EQ(res.tv_sec, static_cast<time_t>(0));
  EXPECT_GT(res.tv_nsec, static_cast<long>(0));
}

// POSIX: null res pointer is valid (function just validates clockid).
TEST_F(LlvmLibcClockGetresTest, NullResIsValid) {
  EXPECT_THAT(LIBC_NAMESPACE::clock_getres(CLOCK_REALTIME, nullptr), Succeeds());
}

// Invalid clock ID returns -1 and sets EINVAL.
TEST_F(LlvmLibcClockGetresTest, InvalidClockId) {
  struct timespec res;
  EXPECT_THAT(LIBC_NAMESPACE::clock_getres(-1, &res), Fails(EINVAL));
}
