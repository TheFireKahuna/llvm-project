//===-- Windows unittests for gettimeofday --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/time/gettimeofday.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/types/struct_timeval.h"
#include "hdr/types/suseconds_t.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcGettimeofdayTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// Returns a plausible wall-clock time with valid sub-second range.
TEST_F(LlvmLibcGettimeofdayTest, PlausibleTime) {
  struct timeval tv;
  ASSERT_THAT(LIBC_NAMESPACE::gettimeofday(&tv, nullptr), Succeeds());
  // After 2020-01-01 00:00:00 UTC.
  EXPECT_GT(tv.tv_sec, static_cast<time_t>(1577836800));
  EXPECT_GE(tv.tv_usec, static_cast<suseconds_t>(0));
  EXPECT_LT(tv.tv_usec, static_cast<suseconds_t>(1000000));
}

// Two consecutive calls are non-decreasing.
TEST_F(LlvmLibcGettimeofdayTest, NonDecreasing) {
  struct timeval t1, t2;
  ASSERT_THAT(LIBC_NAMESPACE::gettimeofday(&t1, nullptr), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::gettimeofday(&t2, nullptr), Succeeds());
  bool non_decreasing = (t2.tv_sec > t1.tv_sec) ||
                        (t2.tv_sec == t1.tv_sec && t2.tv_usec >= t1.tv_usec);
  EXPECT_TRUE(non_decreasing);
}

// Null tz argument is valid (POSIX permits it).
TEST_F(LlvmLibcGettimeofdayTest, NullTzValid) {
  struct timeval tv;
  EXPECT_THAT(LIBC_NAMESPACE::gettimeofday(&tv, nullptr), Succeeds());
}
