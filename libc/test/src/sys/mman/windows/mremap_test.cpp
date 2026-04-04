//===-- Unittests for mremap on Windows ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/mremap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMremapTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcMremapTest, GrowWithMayMove) {
  size_t initial_size = 4096;
  size_t new_size = 2 * 4096;

  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, initial_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  int *array = reinterpret_cast<int *>(addr);
  array[0] = 0xCAFE;
  array[1] = 0xBEEF;

  void *new_addr =
      LIBC_NAMESPACE::mremap(addr, initial_size, new_size, MREMAP_MAYMOVE);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(new_addr, MAP_FAILED);

  // Data must be preserved across remap.
  int *new_array = reinterpret_cast<int *>(new_addr);
  EXPECT_EQ(new_array[0], 0xCAFE);
  EXPECT_EQ(new_array[1], 0xBEEF);

  // New pages should be zero-filled.
  char *bytes = reinterpret_cast<char *>(new_addr);
  EXPECT_EQ(bytes[new_size - 1], static_cast<char>(0));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(new_addr, new_size), Succeeds());
}

TEST_F(LlvmLibcMremapTest, Shrink) {
  size_t initial_size = 4 * 4096;
  size_t new_size = 4096;

  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, initial_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  int *array = reinterpret_cast<int *>(addr);
  array[0] = 777;

  // Shrink — must not move when shrinking.
  void *new_addr =
      LIBC_NAMESPACE::mremap(addr, initial_size, new_size, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(new_addr, addr);
  EXPECT_EQ(reinterpret_cast<int *>(new_addr)[0], 777);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(new_addr, new_size), Succeeds());
}

TEST_F(LlvmLibcMremapTest, SameSize) {
  size_t alloc_size = 4096;

  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  reinterpret_cast<int *>(addr)[0] = 42;

  // Same size, no flags — should return same address.
  void *new_addr =
      LIBC_NAMESPACE::mremap(addr, alloc_size, alloc_size, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_EQ(new_addr, addr);
  EXPECT_EQ(reinterpret_cast<int *>(new_addr)[0], 42);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(new_addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMremapTest, GrowMultiPage) {
  // Grow from 1 page to 8 pages, verify all original data preserved.
  size_t page_size = 4096;
  size_t initial_size = page_size;
  size_t new_size = 8 * page_size;

  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, initial_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Fill the original page with a pattern.
  char *bytes = reinterpret_cast<char *>(addr);
  for (size_t i = 0; i < initial_size; ++i)
    bytes[i] = static_cast<char>(i & 0xFF);

  void *new_addr =
      LIBC_NAMESPACE::mremap(addr, initial_size, new_size, MREMAP_MAYMOVE);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(new_addr, MAP_FAILED);

  // Verify original data.
  char *new_bytes = reinterpret_cast<char *>(new_addr);
  bool data_ok = true;
  for (size_t i = 0; i < initial_size; ++i) {
    if (new_bytes[i] != static_cast<char>(i & 0xFF)) {
      data_ok = false;
      break;
    }
  }
  EXPECT_TRUE(data_ok);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(new_addr, new_size), Succeeds());
}

TEST_F(LlvmLibcMremapTest, Error_InvalidNewSize) {
  size_t initial_size = 4096;

  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, initial_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  void *new_addr =
      LIBC_NAMESPACE::mremap(addr, initial_size, 0, MREMAP_MAYMOVE);
  EXPECT_THAT(new_addr, Fails(EINVAL, MAP_FAILED));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, initial_size), Succeeds());
}
