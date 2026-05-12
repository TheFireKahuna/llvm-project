//===-- Windows unittests for memfd_create --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - memfd_create returns a valid fd; close succeeds
//   - ftruncate + mmap(MAP_SHARED) gives a readable/writable anonymous region
//   - two MAP_SHARED mappings on the same fd share the same backing storage
//   - MFD_CLOEXEC sets FD_CLOEXEC on the returned fd
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/fcntl.h"
#include "src/sys/mman/memfd_create.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/unistd/close.h"
#include "src/unistd/ftruncate.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/sys/mman/windows/test_utils.h"

#include "hdr/fcntl_macros.h"
#include "hdr/types/off_t.h"
#include <sys/mman.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::mman_test_utils::make_unique_name;
using LlvmLibcWindowsMemfdCreateTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// memfd_create returns a usable fd; ftruncate + mmap round-trips data.
TEST_F(LlvmLibcWindowsMemfdCreateTest, MapShared) {
  char name[64];
  ASSERT_TRUE(make_unique_name("test_memfd", name, sizeof(name)));
  int fd = LIBC_NAMESPACE::memfd_create(name, 0);
  ASSERT_GT(fd, 0);

  constexpr off_t SIZE = 4096;
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, SIZE), Succeeds(0));

  void *p = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
  ASSERT_NE(p, MAP_FAILED);

  auto *data = static_cast<uint32_t *>(p);
  *data = 0xDEADBEEF;
  EXPECT_EQ(*data, uint32_t(0xDEADBEEF));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(p, SIZE), Succeeds(0));
  LIBC_NAMESPACE::close(fd);
}

// Two MAP_SHARED mappings on the same fd must share backing storage.
TEST_F(LlvmLibcWindowsMemfdCreateTest, TwoMappingsShareData) {
  char name[64];
  ASSERT_TRUE(make_unique_name("test_memfd_shared", name, sizeof(name)));
  int fd = LIBC_NAMESPACE::memfd_create(name, 0);
  ASSERT_GT(fd, 0);

  constexpr off_t SIZE = 4096;
  ASSERT_THAT(LIBC_NAMESPACE::ftruncate(fd, SIZE), Succeeds(0));

  void *a = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
  void *b = LIBC_NAMESPACE::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
  ASSERT_NE(a, MAP_FAILED);
  ASSERT_NE(b, MAP_FAILED);
  ASSERT_NE(a, b);

  // Write via `a`, read via `b`.
  *static_cast<uint32_t *>(a) = 0xCAFEBABE;
  EXPECT_EQ(*static_cast<uint32_t *>(b), uint32_t(0xCAFEBABE));

  LIBC_NAMESPACE::munmap(a, SIZE);
  LIBC_NAMESPACE::munmap(b, SIZE);
  LIBC_NAMESPACE::close(fd);
}

// MFD_CLOEXEC must set FD_CLOEXEC on the returned fd.
TEST_F(LlvmLibcWindowsMemfdCreateTest, CloexecFlag) {
  char name[64];
  ASSERT_TRUE(make_unique_name("test_memfd_cloexec", name, sizeof(name)));
  int fd = LIBC_NAMESPACE::memfd_create(name, MFD_CLOEXEC);
  ASSERT_GT(fd, 0);

  int flags = LIBC_NAMESPACE::fcntl(fd, F_GETFD);
  EXPECT_NE(flags, -1);
  EXPECT_NE(flags & FD_CLOEXEC, 0);

  LIBC_NAMESPACE::close(fd);
}

// memfd_create without MFD_CLOEXEC must NOT set FD_CLOEXEC.
TEST_F(LlvmLibcWindowsMemfdCreateTest, NoCloexecByDefault) {
  char name[64];
  ASSERT_TRUE(make_unique_name("test_memfd_nocloexec", name, sizeof(name)));
  int fd = LIBC_NAMESPACE::memfd_create(name, 0);
  ASSERT_GT(fd, 0);

  int flags = LIBC_NAMESPACE::fcntl(fd, F_GETFD);
  EXPECT_NE(flags, -1);
  EXPECT_EQ(flags & FD_CLOEXEC, 0);

  LIBC_NAMESPACE::close(fd);
}
