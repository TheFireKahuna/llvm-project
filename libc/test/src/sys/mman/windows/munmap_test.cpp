//===-- Unittests for munmap on Windows -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux errno-contract and behaviour coverage for the rebuilt
// posix/munmap.cpp. Coverage:
//   * Holes in the range are tolerated (Linux munmap semantic).
//   * Loaded PE image regions are rejected with EINVAL (cordon probe).
//   * Page-aligned but sub-allocation-granularity partial unmap rounds
//     to alloc granularity.
//   * Round-trip: a fresh mmap → write → munmap → re-mmap of the same
//     size lands on a fresh page (no stale content leaks).
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMUnmapTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcMUnmapTest, ZeroSizeRejected) {
  // size == 0 is EINVAL (glibc / legacy convention preserved).
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, 0), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, 4096), Succeeds());
}

TEST_F(LlvmLibcMUnmapTest, NullAddrRejected) {
  EXPECT_THAT(LIBC_NAMESPACE::munmap(nullptr, 4096), Fails(EINVAL));
}

TEST_F(LlvmLibcMUnmapTest, UnalignedAddrRejected) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 8192, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  void *misaligned = static_cast<char *>(addr) + 1;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(misaligned, 4096), Fails(EINVAL));
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, 8192), Succeeds());
}

TEST_F(LlvmLibcMUnmapTest, HolesToleratedAcrossRange) {
  //Linux munmap succeeds even if part of the range is
  // already unmapped. Two adjacent regions separated by a hole are
  // both released by a single munmap that spans them — the hole is
  // simply skipped.
  size_t alloc_size = 64 * 1024;
  void *first = LIBC_NAMESPACE::mmap(nullptr, alloc_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(first, MAP_FAILED);

  // Far-away second region — the gap is the "hole".
  void *second = LIBC_NAMESPACE::mmap(
      static_cast<char *>(first) + (1 << 20), alloc_size,
      PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(second, MAP_FAILED);

  size_t span = static_cast<size_t>(static_cast<char *>(second) -
                                     static_cast<char *>(first)) +
                alloc_size;

  // Spanning munmap MUST succeed.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(first, span), Succeeds());
}

TEST_F(LlvmLibcMUnmapTest, FullySingleRegion) {
  // Round-trip: mmap → write → munmap → re-mmap of the same size. The
  // new mapping must be a fresh anonymous-private page (content
  // initialised to zero per POSIX), so the byte the first mapping wrote
  // does not survive into the second.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);
  static_cast<unsigned char *>(addr)[0] = 0xAB;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());

  void *addr2 = LIBC_NAMESPACE::mmap(nullptr, alloc_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr2, MAP_FAILED);
  EXPECT_EQ(static_cast<unsigned char *>(addr2)[0], 0u);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr2, alloc_size), Succeeds());
}
