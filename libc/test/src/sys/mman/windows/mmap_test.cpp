//===-- Unittests for mmap and munmap on Windows ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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
using LlvmLibcMMapTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcMMapTest, AnonymousReadOnly) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // MAP_ANONYMOUS memory is zero-initialized.
  int *array = reinterpret_cast<int *>(addr);
  EXPECT_EQ(array[0], 0);
  EXPECT_EQ(array[alloc_size / sizeof(int) - 1], 0);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, AnonymousReadWrite) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Write and read back.
  int *array = reinterpret_cast<int *>(addr);
  array[0] = 0xDEADBEEF;
  array[1] = 42;
  EXPECT_EQ(array[0], static_cast<int>(0xDEADBEEF));
  EXPECT_EQ(array[1], 42);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, AnonymousProtNone) {
  // PROT_NONE creates a placeholder reservation with no access.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_NONE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Can't read or write — just verify it reserved and can be freed.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, MultiPageAllocation) {
  // Allocate multiple pages and verify the full range is usable.
  size_t alloc_size = 16 * 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(addr);
  // Write to first, middle, and last pages.
  bytes[0] = 'A';
  bytes[alloc_size / 2] = 'B';
  bytes[alloc_size - 1] = 'C';
  EXPECT_EQ(bytes[0], 'A');
  EXPECT_EQ(bytes[alloc_size / 2], 'B');
  EXPECT_EQ(bytes[alloc_size - 1], 'C');

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, MapFixedNoreplace) {
  // Allocate a region, then verify MAP_FIXED_NOREPLACE fails with EEXIST
  // when targeting the same address.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  void *overlap = LIBC_NAMESPACE::mmap(addr, alloc_size, PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE |
                                           MAP_FIXED_NOREPLACE,
                                       -1, 0);
  EXPECT_THAT(overlap, Fails(EEXIST, MAP_FAILED));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, MapFixed) {
  // Allocate a region, then MAP_FIXED overlay to replace it.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Write a sentinel value.
  int *array = reinterpret_cast<int *>(addr);
  array[0] = 0x12345678;

  // MAP_FIXED replaces the existing mapping — memory should be zero-filled.
  void *fixed = LIBC_NAMESPACE::mmap(addr, alloc_size, PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                                     -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(fixed, addr);

  // Old data should be gone.
  EXPECT_EQ(array[0], 0);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, PartialMunmap) {
  // Allocate two pages, unmap the second, verify the first is intact.
  size_t page_size = 4096;
  size_t alloc_size = 2 * page_size;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(addr);
  bytes[0] = 'X';
  bytes[page_size] = 'Y';

  // Unmap second page only.
  EXPECT_THAT(
      LIBC_NAMESPACE::munmap(reinterpret_cast<void *>(bytes + page_size),
                             page_size),
      Succeeds());

  // First page should still be readable.
  EXPECT_EQ(bytes[0], 'X');

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, page_size), Succeeds());
}

TEST_F(LlvmLibcMMapTest, Error_InvalidSize) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 0, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  EXPECT_THAT(addr, Fails(EINVAL, MAP_FAILED));
}

TEST_F(LlvmLibcMMapTest, Error_MunmapZeroSize) {
  EXPECT_THAT(LIBC_NAMESPACE::munmap(nullptr, 0), Fails(EINVAL));
}

TEST_F(LlvmLibcMMapTest, Error_MunmapNullptr) {
  // munmap of nullptr with non-zero size should fail.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(nullptr, 4096), Fails(EINVAL));
}

TEST_F(LlvmLibcMMapTest, Error_InvalidFlags) {
  // Neither MAP_PRIVATE nor MAP_SHARED.
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ, MAP_ANONYMOUS,
                                    -1, 0);
  EXPECT_THAT(addr, Fails(EINVAL, MAP_FAILED));
}
