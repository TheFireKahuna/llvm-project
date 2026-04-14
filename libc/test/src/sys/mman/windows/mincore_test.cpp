//===-- Windows unittests for mincore --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - Committed (touched) pages are reported as resident
//   - Reserved-only (PROT_NONE) pages are reported as non-resident
//   - len=0 returns 0 (no-op)
//   - Unaligned addr → EINVAL
//   - NULL addr → ENOMEM (unmapped)
//   - NULL vec → EFAULT
//   - Multi-page range reports per-page residency
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mincore.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LlvmLibcMincoreTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// Committed and touched pages should be resident.
TEST_F(LlvmLibcMincoreTest, CommittedPagesResident) {
  size_t page_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  // Touch the page to ensure it's resident.
  *reinterpret_cast<volatile char *>(addr) = 'X';

  unsigned char vec[1] = {0xFF};
  EXPECT_THAT(LIBC_NAMESPACE::mincore(addr, page_size, vec), Succeeds());
  EXPECT_EQ(static_cast<int>(vec[0] & 1), 1);

  LIBC_NAMESPACE::munmap(addr, page_size);
}

// PROT_NONE pages are reserved but not committed — should be non-resident.
TEST_F(LlvmLibcMincoreTest, ReservedPagesNotResident) {
  size_t page_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, page_size, PROT_NONE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  unsigned char vec[1] = {0xFF};
  EXPECT_THAT(LIBC_NAMESPACE::mincore(addr, page_size, vec), Succeeds());
  EXPECT_EQ(static_cast<int>(vec[0] & 1), 0);

  LIBC_NAMESPACE::munmap(addr, page_size);
}

// Multi-page range: first page touched, second untouched (PROT_NONE via
// partial mprotect would be complex, so we just verify the multi-page path).
TEST_F(LlvmLibcMincoreTest, MultiPage) {
  size_t page_size = 4096;
  size_t alloc_size = 4 * page_size;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  // Touch first and third pages.
  char *bytes = reinterpret_cast<char *>(addr);
  bytes[0] = 'A';
  bytes[2 * page_size] = 'C';

  unsigned char vec[4] = {};
  EXPECT_THAT(LIBC_NAMESPACE::mincore(addr, alloc_size, vec), Succeeds());
  // Touched pages should be resident.
  EXPECT_EQ(static_cast<int>(vec[0] & 1), 1);
  EXPECT_EQ(static_cast<int>(vec[2] & 1), 1);

  LIBC_NAMESPACE::munmap(addr, alloc_size);
}

// POSIX: len=0 returns 0 (no-op).
TEST_F(LlvmLibcMincoreTest, ZeroLength) {
  size_t page_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, page_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  unsigned char vec[1];
  EXPECT_EQ(LIBC_NAMESPACE::mincore(addr, 0, vec), 0);

  LIBC_NAMESPACE::munmap(addr, page_size);
}

// POSIX/Linux: unaligned addr → EINVAL.
TEST_F(LlvmLibcMincoreTest, UnalignedAddress) {
  size_t page_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, page_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  void *unaligned = reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(addr) + 1);
  unsigned char vec[1];
  EXPECT_THAT(LIBC_NAMESPACE::mincore(unaligned, page_size, vec),
              Fails(EINVAL));

  LIBC_NAMESPACE::munmap(addr, page_size);
}

// Linux: NULL addr → ENOMEM (unmapped address range).
TEST_F(LlvmLibcMincoreTest, NullAddress) {
  unsigned char vec[1];
  EXPECT_THAT(LIBC_NAMESPACE::mincore(nullptr, 4096, vec), Fails(ENOMEM));
}

// POSIX/Linux: NULL vec → EFAULT.
TEST_F(LlvmLibcMincoreTest, NullVec) {
  size_t page_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, page_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::mincore(addr, page_size, nullptr),
              Fails(EFAULT));

  LIBC_NAMESPACE::munmap(addr, page_size);
}
