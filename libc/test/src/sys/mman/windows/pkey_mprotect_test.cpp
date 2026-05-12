//===-- Unittests for pkey_mprotect on Windows ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux errno-contract coverage for the rebuilt pkey_mprotect path.
// Covers the entry behaviours that are independent of whether the host
// CPU has PKU support, plus the x86_64-only allocation/registration
// gates.
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/sys/mman/pkey_alloc.h"
#include "src/sys/mman/pkey_free.h"
#include "src/sys/mman/pkey_mprotect.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcPkeyMprotectTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcPkeyMprotectTest, NullAddrZeroSizeSucceeds) {
  //addr == NULL && size == 0 is a no-op success regardless
  // of architecture — preserved through pkey_alloc-free for portable
  // app probes.
  EXPECT_THAT(LIBC_NAMESPACE::pkey_mprotect(nullptr, 0, PROT_READ, -1),
              Succeeds());
}

TEST_F(LlvmLibcPkeyMprotectTest, NullAddrNonzeroSizeFails) {
  //addr == NULL && size > 0 → EINVAL.
  EXPECT_THAT(LIBC_NAMESPACE::pkey_mprotect(nullptr, 4096, PROT_READ, -1),
              Fails(EINVAL));
}

TEST_F(LlvmLibcPkeyMprotectTest, MinusOnePkeyDelegatesToMprotect) {
  //pkey == -1 is equivalent to plain mprotect — the
  // registration tail is skipped. Architecture-independent.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::pkey_mprotect(addr, alloc_size,
                                            PROT_READ | PROT_WRITE, -1),
              Succeeds());
  static_cast<int *>(addr)[0] = 1;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcPkeyMprotectTest, BasePrececiseErrnoPropagates) {
  // The base mprotect's W+X → EACCES errno must propagate through
  // pkey_mprotect; the pkey registration never runs.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(
      LIBC_NAMESPACE::pkey_mprotect(addr, alloc_size,
                                    PROT_READ | PROT_WRITE | PROT_EXEC, -1),
      Fails(EACCES));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

#ifdef LIBC_TARGET_ARCH_IS_X86_64

TEST_F(LlvmLibcPkeyMprotectTest, UnallocatedPkeyRejected) {
  //an out-of-range or unallocated pkey is EINVAL — the
  // allocated-bitmap check fires before the range registration.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Pkey 14 is overwhelmingly likely to be unallocated on a fresh
  // process — the allocator hands out from key 1 upward. The test
  // tolerates the rare case where a fixture has consumed key 14 by
  // skipping when alloc returns a matching key.
  int probe = LIBC_NAMESPACE::pkey_alloc(0, 0);
  if (probe == 14) {
    LIBC_NAMESPACE::pkey_free(probe);
    GTEST_SKIP();
  } else if (probe >= 0) {
    LIBC_NAMESPACE::pkey_free(probe);
  }

  EXPECT_THAT(
      LIBC_NAMESPACE::pkey_mprotect(addr, alloc_size, PROT_READ, 14),
      Fails(EINVAL));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcPkeyMprotectTest, AllocatedPkeyRegisters) {
  //the happy-path tail runs `pkey_register_range` and
  // returns 0. Pair with pkey_free so the test is clean.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  int key = LIBC_NAMESPACE::pkey_alloc(0, 0);
  if (key < 0) {
    // Process is already out of pkeys; can't exercise the success
    // path here.
    EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
    GTEST_SKIP();
  }

  EXPECT_THAT(
      LIBC_NAMESPACE::pkey_mprotect(addr, alloc_size, PROT_READ, key),
      Succeeds());

  LIBC_NAMESPACE::pkey_free(key);
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

#endif // LIBC_TARGET_ARCH_IS_X86_64
