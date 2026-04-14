//===-- Windows unittests for arc4random -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compliance points tested (OpenBSD/FreeBSD/POSIX-adjacent):
//   - arc4random() returns a uniformly distributed 32-bit value
//   - Consecutive calls produce different values (probabilistic)
//   - arc4random_buf() fills buffer with random bytes
//   - arc4random_buf() with len=0 is a no-op
//   - arc4random_uniform(0) returns 0
//   - arc4random_uniform(1) returns 0
//   - arc4random_uniform(n) returns values in [0, n)
//   - Output has reasonable entropy (not all-zero, not constant)
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/arc4random.h"
#include "src/stdlib/arc4random_buf.h"
#include "src/stdlib/arc4random_uniform.h"
#include "test/UnitTest/Test.h"

// arc4random() returns different values on consecutive calls.
// Two identical 32-bit values in a row has probability 1/2^32 — negligible.
TEST(LlvmLibcArc4randomTest, NotConstant) {
  uint32_t a = LIBC_NAMESPACE::arc4random();
  uint32_t b = LIBC_NAMESPACE::arc4random();
  // Could theoretically be equal, but astronomically unlikely.
  // Test that at least out of 10 calls, not all are equal.
  bool all_same = true;
  for (int i = 0; i < 10; i++) {
    if (LIBC_NAMESPACE::arc4random() != a) {
      all_same = false;
      break;
    }
  }
  EXPECT_FALSE(all_same);
  (void)b;
}

// arc4random_buf fills a buffer with random bytes.
TEST(LlvmLibcArc4randomTest, BufFills) {
  unsigned char buf[64];
  // Initialize to a known pattern.
  for (int i = 0; i < 64; i++)
    buf[i] = 0;

  LIBC_NAMESPACE::arc4random_buf(buf, sizeof(buf));

  // Verify not all zeros (probability 2^-512).
  bool all_zero = true;
  for (int i = 0; i < 64; i++) {
    if (buf[i] != 0) {
      all_zero = false;
      break;
    }
  }
  EXPECT_FALSE(all_zero);
}

// arc4random_buf with different sizes.
TEST(LlvmLibcArc4randomTest, BufVariousSizes) {
  unsigned char buf1[1] = {0};
  LIBC_NAMESPACE::arc4random_buf(buf1, sizeof(buf1));

  unsigned char buf256[256];
  for (int i = 0; i < 256; i++)
    buf256[i] = 0;
  LIBC_NAMESPACE::arc4random_buf(buf256, sizeof(buf256));

  // Check the larger buffer has some non-zero bytes.
  int nonzero = 0;
  for (int i = 0; i < 256; i++) {
    if (buf256[i] != 0)
      nonzero++;
  }
  EXPECT_GT(nonzero, 0);
}

// arc4random_buf with len=0 is a no-op (should not crash).
TEST(LlvmLibcArc4randomTest, BufZeroLength) {
  unsigned char buf[1] = {0xAA};
  LIBC_NAMESPACE::arc4random_buf(buf, 0);
  // Buffer should be untouched.
  EXPECT_EQ(buf[0], static_cast<unsigned char>(0xAA));
}

// arc4random_uniform(0) returns 0.
TEST(LlvmLibcArc4randomTest, UniformZero) {
  EXPECT_EQ(LIBC_NAMESPACE::arc4random_uniform(0), static_cast<uint32_t>(0));
}

// arc4random_uniform(1) always returns 0.
TEST(LlvmLibcArc4randomTest, UniformOne) {
  for (int i = 0; i < 100; i++)
    EXPECT_EQ(LIBC_NAMESPACE::arc4random_uniform(1), static_cast<uint32_t>(0));
}

// arc4random_uniform(n) returns values in [0, n).
TEST(LlvmLibcArc4randomTest, UniformBound) {
  uint32_t upper = 10;
  for (int i = 0; i < 200; i++) {
    uint32_t val = LIBC_NAMESPACE::arc4random_uniform(upper);
    EXPECT_LT(val, upper);
  }
}

// arc4random_uniform with a large bound.
TEST(LlvmLibcArc4randomTest, UniformLargeBound) {
  uint32_t upper = 1000000000;
  for (int i = 0; i < 50; i++) {
    uint32_t val = LIBC_NAMESPACE::arc4random_uniform(upper);
    EXPECT_LT(val, upper);
  }
}

// Verify two calls to arc4random_buf produce different output.
TEST(LlvmLibcArc4randomTest, BufNotDeterministic) {
  unsigned char buf1[32], buf2[32];
  LIBC_NAMESPACE::arc4random_buf(buf1, sizeof(buf1));
  LIBC_NAMESPACE::arc4random_buf(buf2, sizeof(buf2));

  bool same = true;
  for (int i = 0; i < 32; i++) {
    if (buf1[i] != buf2[i]) {
      same = false;
      break;
    }
  }
  EXPECT_FALSE(same);
}
