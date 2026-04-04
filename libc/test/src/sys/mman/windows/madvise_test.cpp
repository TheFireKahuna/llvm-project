//===-- Unittests for madvise on Windows -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/madvise.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMadviseTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcMadviseTest, DontneedZeroFill) {
  // MADV_DONTNEED on private anonymous memory must zero-fill.
  size_t alloc_size = 4096;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Dirty the page.
  char *bytes = reinterpret_cast<char *>(addr);
  for (size_t i = 0; i < alloc_size; ++i)
    bytes[i] = static_cast<char>(0xAA);
  EXPECT_EQ(bytes[0], static_cast<char>(0xAA));

  // MADV_DONTNEED should succeed.
  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_DONTNEED),
              Succeeds());

  // Memory must be zero after MADV_DONTNEED.
  EXPECT_EQ(bytes[0], static_cast<char>(0));
  EXPECT_EQ(bytes[alloc_size - 1], static_cast<char>(0));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, FreeOnPrivate) {
  // MADV_FREE marks pages as reclaimable but doesn't zero them immediately.
  size_t alloc_size = 4096;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(addr);
  bytes[0] = 'Z';

  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_FREE),
              Succeeds());

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, Willneed) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_WILLNEED),
              Succeeds());

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, SequentialRandomNormal) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_SEQUENTIAL),
              Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_RANDOM),
              Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_NORMAL),
              Succeeds());

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, HugepageNoOp) {
  // MADV_HUGEPAGE and MADV_NOHUGEPAGE are no-ops on Windows.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_HUGEPAGE),
              Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_NOHUGEPAGE),
              Succeeds());

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, ColdPages) {
  size_t alloc_size = 4096;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Touch the page then mark cold.
  reinterpret_cast<char *>(addr)[0] = 1;
  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_COLD),
              Succeeds());

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, DontneedPreservesProtection) {
  // After MADV_DONTNEED, the protection bits should remain intact.
  size_t alloc_size = 4096;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(addr);
  for (size_t i = 0; i < alloc_size; ++i)
    bytes[i] = static_cast<char>(0xFF);

  EXPECT_THAT(LIBC_NAMESPACE::madvise(addr, alloc_size, MADV_DONTNEED),
              Succeeds());

  // Should still be writable after DONTNEED (protection preserved).
  bytes[0] = 'A';
  EXPECT_EQ(bytes[0], 'A');

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMadviseTest, Error_BadPtr) {
  EXPECT_THAT(LIBC_NAMESPACE::madvise(nullptr, 4096, MADV_SEQUENTIAL),
              Fails(ENOMEM));
}
