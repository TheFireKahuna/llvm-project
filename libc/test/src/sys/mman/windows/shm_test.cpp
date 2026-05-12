//===-- Windows unittests for shm_open/shm_unlink -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - shm_open() creates a shared memory object, returns valid fd
//   - shm_open() with O_CREAT|O_EXCL fails with EEXIST for existing object
//   - shm_unlink() removes the shared memory object
//   - shm_unlink() of nonexistent name → ENOENT
//   - Name validation: must start with '/', no embedded '/'
//   - shm_open() with invalid name → EINVAL
//   - mmap on shm fd allows shared read/write
//   - ftruncate sets the size of the shared memory object
//
//===----------------------------------------------------------------------===//

#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "src/sys/mman/shm_open.h"
#include "src/sys/mman/shm_unlink.h"
#include "src/unistd/close.h"
#include "src/unistd/ftruncate.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/sys/mman/windows/test_utils.h"

#include "include/llvm-libc-macros/fcntl-macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"

using LlvmLibcShmTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LIBC_NAMESPACE::mman_test_utils::make_unique_name;
using LIBC_NAMESPACE::mman_test_utils::ShmUnlinkGuard;

// POSIX: shm_open() creates and opens a shared memory object.
TEST_F(LlvmLibcShmTest, CreateAndUnlink) {
  char name[64];
  ASSERT_TRUE(make_unique_name("/llvm_libc_shm_test_basic", name, sizeof(name)));
  ShmUnlinkGuard guard(name);

  int fd = LIBC_NAMESPACE::shm_open(name, O_CREAT | O_RDWR, 0600);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_GE(fd, 0);

  EXPECT_THAT(LIBC_NAMESPACE::close(fd), Succeeds());
}

// POSIX: "O_CREAT|O_EXCL — If [...] the named shared memory object already
// exists, [...] fail and set errno to [EEXIST]."
TEST_F(LlvmLibcShmTest, ExclusiveCreate) {
  char name[64];
  ASSERT_TRUE(make_unique_name("/llvm_libc_shm_test_excl", name, sizeof(name)));
  ShmUnlinkGuard guard(name);

  int fd1 = LIBC_NAMESPACE::shm_open(name, O_CREAT | O_RDWR, 0600);
  ASSERT_GE(fd1, 0);

  // Second create with O_EXCL should fail.
  int fd2 = LIBC_NAMESPACE::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  EXPECT_THAT(fd2, Fails(EEXIST));

  LIBC_NAMESPACE::close(fd1);
}

// POSIX: shm_unlink of nonexistent name → ENOENT.
TEST_F(LlvmLibcShmTest, UnlinkNonexistent) {
  EXPECT_THAT(LIBC_NAMESPACE::shm_unlink("/llvm_libc_shm_nonexistent_xyzzy"),
              Fails(ENOENT));
}

// POSIX: Name must begin with '/'. No other '/' allowed.
TEST_F(LlvmLibcShmTest, InvalidName) {
  // No leading slash.
  EXPECT_THAT(LIBC_NAMESPACE::shm_open("no_slash", O_CREAT | O_RDWR, 0600),
              Fails(EINVAL));

  // Embedded slash.
  EXPECT_THAT(LIBC_NAMESPACE::shm_open("/has/slash", O_CREAT | O_RDWR, 0600),
              Fails(EINVAL));

  // Just "/" (body is empty).
  EXPECT_THAT(LIBC_NAMESPACE::shm_open("/", O_CREAT | O_RDWR, 0600),
              Fails(EINVAL));
}

// POSIX: mmap on shm fd allows read/write, ftruncate sets size.
TEST_F(LlvmLibcShmTest, MmapOnShmFd) {
  char name[64];
  ASSERT_TRUE(make_unique_name("/llvm_libc_shm_test_mmap", name, sizeof(name)));
  ShmUnlinkGuard guard(name);
  size_t shm_size = 4096;

  int fd = LIBC_NAMESPACE::shm_open(name, O_CREAT | O_RDWR, 0600);
  ASSERT_GE(fd, 0);

  // Set size.
  EXPECT_THAT(LIBC_NAMESPACE::ftruncate(fd, shm_size), Succeeds());

  // Map the shared memory.
  void *addr = LIBC_NAMESPACE::mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
  ASSERT_NE(addr, MAP_FAILED);

  // Write and read back.
  int *data = reinterpret_cast<int *>(addr);
  data[0] = 0x12345678;
  EXPECT_EQ(data[0], 0x12345678);

  EXPECT_THAT(LIBC_NAMESPACE::munmap(addr, shm_size), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::close(fd), Succeeds());
}
