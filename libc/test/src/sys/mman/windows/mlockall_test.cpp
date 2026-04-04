//===-- Windows unittests for mlockall/munlockall/mlock2 ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - mlockall(MCL_CURRENT) returns 0 and data remains accessible
//   - mlockall(MCL_FUTURE) returns 0; future mmap pages are accessible
//   - mlockall(MCL_CURRENT|MCL_FUTURE) returns 0
//   - mlockall(MCL_ONFAULT|MCL_CURRENT) returns 0
//   - mlockall(MCL_ONFAULT alone) returns -1, errno=EINVAL
//   - mlockall(0) returns -1, errno=EINVAL
//   - mlockall with unknown flags returns -1, errno=EINVAL
//   - munlockall() returns 0 unconditionally
//   - mlock2 with flags=0 matches mlock behaviour
//   - mlock2 with MLOCK_ONFAULT returns 0
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mlock2.h"
#include "src/sys/mman/mlockall.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munlockall.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "include/llvm-libc-macros/sys-mman-macros.h"

using LlvmLibcMlockallTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// MCL_CURRENT: lock all currently committed pages; data remains accessible.
TEST_F(LlvmLibcMlockallTest, MclCurrent) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);
  *static_cast<int *>(addr) = 0x1234;

  EXPECT_THAT(LIBC_NAMESPACE::mlockall(MCL_CURRENT), Succeeds(0));
  EXPECT_EQ(*static_cast<int *>(addr), 0x1234);

  LIBC_NAMESPACE::munlockall();
  LIBC_NAMESPACE::munmap(addr, 4096);
}

// MCL_FUTURE: pages allocated after this call are locked.
TEST_F(LlvmLibcMlockallTest, MclFuture) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(MCL_FUTURE), Succeeds(0));

  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);
  *static_cast<int *>(addr) = 0x5678;
  EXPECT_EQ(*static_cast<int *>(addr), 0x5678);

  LIBC_NAMESPACE::munlockall();
  LIBC_NAMESPACE::munmap(addr, 4096);
}

// MCL_CURRENT | MCL_FUTURE: combined flags.
TEST_F(LlvmLibcMlockallTest, MclCurrentAndFuture) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(MCL_CURRENT | MCL_FUTURE), Succeeds(0));
  LIBC_NAMESPACE::munlockall();
}

// MCL_ONFAULT combined with MCL_CURRENT: valid — deferred locking.
TEST_F(LlvmLibcMlockallTest, MclOnfaultWithCurrent) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(MCL_ONFAULT | MCL_CURRENT), Succeeds(0));
  LIBC_NAMESPACE::munlockall();
}

// MCL_ONFAULT combined with MCL_FUTURE: valid.
TEST_F(LlvmLibcMlockallTest, MclOnfaultWithFuture) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(MCL_ONFAULT | MCL_FUTURE), Succeeds(0));
  LIBC_NAMESPACE::munlockall();
}

// MCL_ONFAULT alone: meaningless without MCL_CURRENT or MCL_FUTURE → EINVAL.
TEST_F(LlvmLibcMlockallTest, MclOnfaultAloneIsEinval) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(MCL_ONFAULT), Fails(EINVAL));
}

// flags=0: EINVAL.
TEST_F(LlvmLibcMlockallTest, ZeroFlagsIsEinval) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(0), Fails(EINVAL));
}

// Unknown flags: EINVAL.
TEST_F(LlvmLibcMlockallTest, UnknownFlagsIsEinval) {
  EXPECT_THAT(LIBC_NAMESPACE::mlockall(0xFF00), Fails(EINVAL));
}

// munlockall returns 0 unconditionally.
TEST_F(LlvmLibcMlockallTest, MunlockallSucceeds) {
  EXPECT_THAT(LIBC_NAMESPACE::munlockall(), Succeeds(0));
}

// mlock2(addr, len, 0): equivalent to mlock.
TEST_F(LlvmLibcMlockallTest, Mlock2FlagsZero) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);
  *static_cast<int *>(addr) = 0xABCD;

  EXPECT_THAT(LIBC_NAMESPACE::mlock2(addr, 4096, 0), Succeeds(0));
  EXPECT_EQ(*static_cast<int *>(addr), 0xABCD);

  LIBC_NAMESPACE::munmap(addr, 4096);
}

// mlock2 with MLOCK_ONFAULT: deferred locking — returns 0.
TEST_F(LlvmLibcMlockallTest, Mlock2Onfault) {
  void *addr = LIBC_NAMESPACE::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(addr, MAP_FAILED);

  EXPECT_THAT(LIBC_NAMESPACE::mlock2(addr, 4096, MLOCK_ONFAULT), Succeeds(0));

  LIBC_NAMESPACE::munmap(addr, 4096);
}
