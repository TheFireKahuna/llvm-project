//===-- Windows unittests for getentropy ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - getentropy fills the buffer and returns 0
//   - the maximum allowed length is 256 bytes
//   - length > 256 returns -1 and sets EIO
//   - two calls are statistically unlikely to return identical data
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getentropy.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsGetentropyTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// getentropy must return 0 and write bytes into the buffer.
TEST_F(LlvmLibcWindowsGetentropyTest, FillsBuffer) {
  unsigned char buf[32] = {};
  EXPECT_THAT(LIBC_NAMESPACE::getentropy(buf, sizeof(buf)), Succeeds(0));
  // Verify not all bytes are zero (all-zero from ProcessPrng is vanishingly
  // unlikely — probability 2^-256).
  bool any_nonzero = false;
  for (size_t i = 0; i < sizeof(buf); ++i)
    if (buf[i] != 0) { any_nonzero = true; break; }
  EXPECT_TRUE(any_nonzero);
}

// Exactly 256 bytes must succeed (the documented maximum).
TEST_F(LlvmLibcWindowsGetentropyTest, MaxLength) {
  unsigned char buf[256] = {};
  EXPECT_THAT(LIBC_NAMESPACE::getentropy(buf, 256), Succeeds(0));
}

// Length > 256 must fail with EIO.
TEST_F(LlvmLibcWindowsGetentropyTest, TooLong) {
  unsigned char buf[257] = {};
  EXPECT_THAT(LIBC_NAMESPACE::getentropy(buf, 257), Fails(EIO));
}

// Two independent calls must return different data (astronomically unlikely
// to collide; this test catches a broken implementation returning zeros).
TEST_F(LlvmLibcWindowsGetentropyTest, NonRepeating) {
  unsigned char a[16] = {}, b[16] = {};
  LIBC_NAMESPACE::getentropy(a, sizeof(a));
  LIBC_NAMESPACE::getentropy(b, sizeof(b));
  EXPECT_NE(__builtin_memcmp(a, b, sizeof(a)), 0);
}
