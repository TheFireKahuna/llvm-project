//===-- Unittests for mprotect on Windows ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/mprotect.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMProtectTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcMProtectTest, ReadToReadWrite) {
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  int *array = reinterpret_cast<int *>(addr);
  EXPECT_EQ(array[0], 0);

  // Upgrade to read-write.
  EXPECT_THAT(
      LIBC_NAMESPACE::mprotect(addr, alloc_size, PROT_READ | PROT_WRITE),
      Succeeds());
  array[0] = 42;
  EXPECT_EQ(array[0], 42);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, ProtNoneThenCommit) {
  // Thread stack pattern: mmap(PROT_NONE) then mprotect() to commit.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_NONE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  // Upgrade from PROT_NONE to read-write — triggers demand-map of placeholder.
  EXPECT_THAT(
      LIBC_NAMESPACE::mprotect(addr, alloc_size, PROT_READ | PROT_WRITE),
      Succeeds());

  // Memory should be accessible and zero-filled after commit.
  int *array = reinterpret_cast<int *>(addr);
  EXPECT_EQ(array[0], 0);
  array[0] = 99;
  EXPECT_EQ(array[0], 99);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, ReadWriteToReadOnly) {
  size_t alloc_size = 4096;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  int *array = reinterpret_cast<int *>(addr);
  array[0] = 123;

  // Downgrade to read-only.
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(addr, alloc_size, PROT_READ),
              Succeeds());

  // Data written before downgrade should still be readable.
  EXPECT_EQ(array[0], 123);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, MultiPageProtection) {
  // Allocate 4 pages, mprotect the middle 2 to a different protection.
  size_t page_size = 4096;
  size_t alloc_size = 4 * page_size;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  char *bytes = reinterpret_cast<char *>(addr);
  bytes[0] = 'A';                     // page 0
  bytes[page_size] = 'B';             // page 1
  bytes[2 * page_size] = 'C';         // page 2
  bytes[3 * page_size] = 'D';         // page 3

  // Make middle 2 pages read-only.
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(bytes + page_size, 2 * page_size,
                                       PROT_READ),
              Succeeds());

  // All data should still be readable.
  EXPECT_EQ(bytes[0], 'A');
  EXPECT_EQ(bytes[page_size], 'B');
  EXPECT_EQ(bytes[2 * page_size], 'C');
  EXPECT_EQ(bytes[3 * page_size], 'D');

  // First and last pages should still be writable.
  bytes[0] = 'Z';
  bytes[3 * page_size] = 'W';
  EXPECT_EQ(bytes[0], 'Z');
  EXPECT_EQ(bytes[3 * page_size], 'W');

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, Error_InvalidAddress) {
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(nullptr, 4096, PROT_READ),
              Fails(EINVAL));
}

// ---------------------------------------------------------------------------
// Linux errno-contract edge cases. These rows have specific errno
// contracts that subtly diverge from "permission denied generally" —
// portable apps test on the exact errno to detect kernel feature gaps.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMProtectTest, ZeroSizeIsNoOp) {
  // size == 0 is a no-op success regardless of `addr` (Linux contract).
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(nullptr, 0, PROT_READ), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, WriteExecRejectedWithEACCES) {
  //W+X is rejected with EACCES (the SELinux-style W^X errno)
  // rather than EINVAL or ENOTSUP — apps distinguish "JIT policy refused"
  // from "argument shape wrong".
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::mprotect(addr, alloc_size,
                                       PROT_READ | PROT_WRITE | PROT_EXEC),
              Fails(EACCES));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, ExecWithoutReadRejectedWithENOTSUP) {
  //Windows lacks an execute-only PTE. Silent promotion to
  // PAGE_EXECUTE_READ would weaken the caller's read-deny intent, so
  // the request is rejected with ENOTSUP.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::mprotect(addr, alloc_size, PROT_EXEC),
              Fails(ENOTSUP));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, UnknownProtBitsRejected) {
  //only PROT_READ / PROT_WRITE / PROT_EXEC are accepted.
  // Forward-compat: silent acceptance of unrecognised bits would mask
  // a missing kernel feature.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  constexpr int kProtGrowsdown = 0x01000000;
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(addr, alloc_size,
                                       PROT_READ | kProtGrowsdown),
              Fails(EINVAL));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, UnalignedAddressRejected) {
  //addr must be page-aligned. Sub-page hints are EINVAL.
  size_t alloc_size = 4096;
  void *addr = LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  void *misaligned = static_cast<char *>(addr) + 1;
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(misaligned, alloc_size, PROT_READ),
              Fails(EINVAL));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, FreeMidRangeReturnsENOMEM) {
  //a MEM_FREE chunk mid-range fails with ENOMEM (distinct
  // from munmap's tolerated holes). First-failure-aborts; chunks
  // already protected stay with the new prot — Linux contract.
  size_t page_size = 4096;
  size_t alloc_size = 4 * page_size;
  void *addr_low =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr_low, MAP_FAILED);

  // Map a second 4-page region far enough away that the kernel does
  // not coalesce it with the first.
  void *addr_high =
      LIBC_NAMESPACE::mmap(static_cast<char *>(addr_low) + (1 << 20),
                           alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr_high, MAP_FAILED);

  // mprotect across both regions — the MEM_FREE between them must
  // surface ENOMEM.
  size_t span =
      static_cast<size_t>(static_cast<char *>(addr_high) - static_cast<char *>(addr_low)) +
      alloc_size;
  EXPECT_THAT(LIBC_NAMESPACE::mprotect(addr_low, span, PROT_READ),
              Fails(ENOMEM));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr_low, alloc_size), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr_high, alloc_size), Succeeds());
}

TEST_F(LlvmLibcMProtectTest, ProtNoneRoundTripPreservesContent) {
  //PAGE_NOACCESS preserves page contents, so
  // mprotect(PROT_NONE) → mprotect(PROT_R/W) recovers the original
  // bytes. The legacy invariant the demand-commit path depends on.
  size_t alloc_size = 4096;
  void *addr =
      LIBC_NAMESPACE::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_ERRNO_SUCCESS();
  EXPECT_NE(addr, MAP_FAILED);

  int *array = reinterpret_cast<int *>(addr);
  array[0] = 0xCAFE;
  array[1] = 0xBABE;

  EXPECT_THAT(LIBC_NAMESPACE::mprotect(addr, alloc_size, PROT_NONE),
              Succeeds());
  EXPECT_THAT(
      LIBC_NAMESPACE::mprotect(addr, alloc_size, PROT_READ | PROT_WRITE),
      Succeeds());

  EXPECT_EQ(array[0], 0xCAFE);
  EXPECT_EQ(array[1], 0xBABE);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, alloc_size), Succeeds());
}
