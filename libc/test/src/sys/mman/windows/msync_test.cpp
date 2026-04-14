//===-- Unittests for msync on Windows -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/msync.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMsyncTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

struct PageHolder {
  static constexpr size_t SIZE = 4096;
  void *addr;

  PageHolder()
      : addr(LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) {}
  ~PageHolder() {
    if (addr != MAP_FAILED)
      LIBC_NAMESPACE::munmap(addr, SIZE);
  }

  char &operator[](size_t i) { return reinterpret_cast<char *>(addr)[i]; }
  bool is_valid() { return addr != MAP_FAILED; }
};

TEST_F(LlvmLibcMsyncTest, SyncAnonymous) {
  PageHolder page;
  ASSERT_TRUE(page.is_valid());

  page[0] = 'A';
  EXPECT_THAT(LIBC_NAMESPACE::msync(page.addr, PageHolder::SIZE, MS_SYNC),
              Succeeds());
}

TEST_F(LlvmLibcMsyncTest, AsyncAnonymous) {
  PageHolder page;
  ASSERT_TRUE(page.is_valid());

  page[0] = 'B';
  EXPECT_THAT(LIBC_NAMESPACE::msync(page.addr, PageHolder::SIZE, MS_ASYNC),
              Succeeds());
}

TEST_F(LlvmLibcMsyncTest, Error_UnmappedMemory) {
  EXPECT_THAT(LIBC_NAMESPACE::msync(nullptr, 1024, MS_SYNC), Fails(ENOMEM));
  EXPECT_THAT(LIBC_NAMESPACE::msync(nullptr, 1024, MS_ASYNC), Fails(ENOMEM));
}

TEST_F(LlvmLibcMsyncTest, Error_InvalidFlags) {
  PageHolder page;
  ASSERT_TRUE(page.is_valid());

  // MS_SYNC | MS_ASYNC is invalid.
  EXPECT_THAT(
      LIBC_NAMESPACE::msync(page.addr, PageHolder::SIZE, MS_SYNC | MS_ASYNC),
      Fails(EINVAL));

  // All bits set is invalid.
  EXPECT_THAT(LIBC_NAMESPACE::msync(page.addr, PageHolder::SIZE, -1),
              Fails(EINVAL));
}

TEST_F(LlvmLibcMsyncTest, Error_UnalignedAddress) {
  PageHolder page;
  ASSERT_TRUE(page.is_valid());

  // Address not page-aligned.
  EXPECT_THAT(
      LIBC_NAMESPACE::msync(&page[1], PageHolder::SIZE - 1, MS_SYNC),
      Fails(EINVAL));
}
