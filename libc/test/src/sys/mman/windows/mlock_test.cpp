//===-- Windows unittests for mlock/munlock --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - mlock() locks committed pages into physical memory, returns 0
//   - munlock() unlocks pages, returns 0
//   - munlock() on non-locked memory is not an error (POSIX)
//   - mlock(NULL, ...) returns -1 with ENOMEM
//   - mlock() with len=0 returns 0 (no-op)
//   - Data written before mlock is preserved after lock/unlock cycle
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mlock.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munlock.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LlvmLibcMlockTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// POSIX: mlock() locks pages; munlock() unlocks. Basic round-trip.
TEST_F(LlvmLibcMlockTest, LockAndUnlock) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(addr, MAP_FAILED);

  // Write data before locking.
  int *data = reinterpret_cast<int *>(addr);
  data[0] = 0xCAFEBABE;

  EXPECT_THAT(LIBC_NAMESPACE::mlock(addr, alloc_size), Succeeds());

  // Data should be preserved.
  EXPECT_EQ(data[0], static_cast<int>(0xCAFEBABE));

  EXPECT_THAT(LIBC_NAMESPACE::munlock(addr, alloc_size), Succeeds());

  // Data still accessible after unlock.
  EXPECT_EQ(data[0], static_cast<int>(0xCAFEBABE));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

// POSIX: mlock with multi-page range.
TEST_F(LlvmLibcMlockTest, MultiPage) {
  size_t alloc_size = 4 * 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(addr, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(addr);
  bytes[0] = 'A';
  bytes[alloc_size - 1] = 'Z';

  EXPECT_THAT(LIBC_NAMESPACE::mlock(addr, alloc_size), Succeeds());
  EXPECT_EQ(bytes[0], 'A');
  EXPECT_EQ(bytes[alloc_size - 1], 'Z');

  EXPECT_THAT(LIBC_NAMESPACE::munlock(addr, alloc_size), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

// POSIX: "munlock() on memory that is not locked is not an error."
TEST_F(LlvmLibcMlockTest, MunlockNotLockedIsNotError) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(addr, MAP_FAILED);

  // Never locked, but munlock should still succeed.
  EXPECT_THAT(LIBC_NAMESPACE::munlock(addr, alloc_size), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

// POSIX: mlock(NULL) → ENOMEM (matching Linux).
TEST_F(LlvmLibcMlockTest, NullAddress) {
  EXPECT_THAT(LIBC_NAMESPACE::mlock(nullptr, 4096), Fails(ENOMEM));
}

// POSIX: munlock(NULL) → ENOMEM.
TEST_F(LlvmLibcMlockTest, MunlockNullAddress) {
  EXPECT_THAT(LIBC_NAMESPACE::munlock(nullptr, 4096), Fails(ENOMEM));
}

// POSIX: mlock with len=0 is a no-op.
TEST_F(LlvmLibcMlockTest, ZeroLength) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  EXPECT_EQ(LIBC_NAMESPACE::mlock(addr, 0), 0);
  EXPECT_EQ(LIBC_NAMESPACE::munlock(addr, 0), 0);

  LIBC_NAMESPACE::munmap(addr, alloc_size);
}
