//===-- Windows unittests for posix_madvise --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - posix_madvise returns the error code directly (not -1/errno)
//   - POSIX_MADV_NORMAL, SEQUENTIAL, RANDOM, WILLNEED all return 0
//   - POSIX_MADV_DONTNEED returns 0 and data remains accessible (POSIX
//     requires no data loss, unlike madvise(MADV_DONTNEED) on Linux)
//   - size=0 is a no-op returning 0
//   - null addr returns EINVAL
//   - unknown advice returns EINVAL
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/sys/mman/posix_madvise.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LlvmLibcPosixMadviseTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// posix_madvise returns error code directly — these helpers make intent clear.
#define EXPECT_MADVISE_OK(advice)                                              \
  EXPECT_EQ(LIBC_NAMESPACE::posix_madvise(addr, 4096, (advice)), 0)
#define EXPECT_MADVISE_ERR(advice, err)                                        \
  EXPECT_EQ(LIBC_NAMESPACE::posix_madvise(addr, 4096, (advice)), (err))

// All access-pattern hints must return 0 on a valid committed mapping.
TEST_F(LlvmLibcPosixMadviseTest, AccessPatternHints) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  EXPECT_MADVISE_OK(POSIX_MADV_NORMAL);
  EXPECT_MADVISE_OK(POSIX_MADV_SEQUENTIAL);
  EXPECT_MADVISE_OK(POSIX_MADV_RANDOM);
  EXPECT_MADVISE_OK(POSIX_MADV_WILLNEED);

  LIBC_NAMESPACE::munmap(addr, 4096);
}

// POSIX_MADV_DONTNEED must preserve data (no data loss, unlike Linux madvise).
TEST_F(LlvmLibcPosixMadviseTest, DontneedPreservesData) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  *static_cast<int *>(addr) = 0xDEADBEEF;

  EXPECT_MADVISE_OK(POSIX_MADV_DONTNEED);

  // Data must remain accessible — POSIX prohibits data loss here.
  EXPECT_EQ(*static_cast<int *>(addr), static_cast<int>(0xDEADBEEF));

  LIBC_NAMESPACE::munmap(addr, 4096);
}

// size=0 is a no-op; returns 0.
TEST_F(LlvmLibcPosixMadviseTest, ZeroSizeIsNoop) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  EXPECT_EQ(LIBC_NAMESPACE::posix_madvise(addr, 0, POSIX_MADV_NORMAL), 0);

  LIBC_NAMESPACE::munmap(addr, 4096);
}

// null addr → EINVAL (returned directly, not via errno).
TEST_F(LlvmLibcPosixMadviseTest, NullAddrIsEinval) {
  void *addr = nullptr;
  EXPECT_MADVISE_ERR(POSIX_MADV_NORMAL, EINVAL);
}

// Unknown advice value → EINVAL.
TEST_F(LlvmLibcPosixMadviseTest, UnknownAdviceIsEinval) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  EXPECT_MADVISE_ERR(999, EINVAL);

  LIBC_NAMESPACE::munmap(addr, 4096);
}
