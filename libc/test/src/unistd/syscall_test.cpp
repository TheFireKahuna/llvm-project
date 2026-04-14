//===-- Unittests for syscalls --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/close.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include <sys/stat.h>    // For S_* flags.
#include <sys/syscall.h> // For syscall numbers.
#include <unistd.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcSyscallTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// We only do a smoke test here. Actual functionality tests are
// done by the unit tests of the syscall wrappers like linkat, readlinkat, and
// pread/pwrite. The goal here is just to exercise the public syscall entry
// point, errno translation, and pointer-bearing arguments.

TEST_F(LlvmLibcSyscallTest, TrivialCall) {
  ASSERT_GE(syscall(SYS_gettid), 0l);
  ASSERT_ERRNO_SUCCESS();
}

TEST_F(LlvmLibcSyscallTest, UnknownCallFailsWithEnosys) {
  ASSERT_THAT(syscall(-1), Fails<long>(ENOSYS));
}

namespace {

long open_test_file(const char *path, int flags, mode_t mode) {
#ifdef SYS_openat
  return syscall(SYS_openat, AT_FDCWD, path, flags, mode);
#elif defined(SYS_open)
  return syscall(SYS_open, path, flags, mode);
#else
#error "open and openat syscalls not available."
#endif
}

} // namespace

TEST_F(LlvmLibcSyscallTest, FileReadWrite) {
  constexpr char HELLO[] = "hello";
  constexpr long HELLO_SIZE = sizeof(HELLO);
  auto TEST_FILE = libc_make_test_file_path("syscall_smoke.test");

  struct ScopedFileCleanup {
    const char *path;
    long fd = -1;

    ~ScopedFileCleanup() {
      if (fd >= 0)
        (void)LIBC_NAMESPACE::close(static_cast<int>(fd));
      (void)LIBC_NAMESPACE::unlink(path);
    }
  } cleanup{TEST_FILE};

  cleanup.fd =
      open_test_file(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU);
  ASSERT_GT(cleanup.fd, 0l);
  ASSERT_ERRNO_SUCCESS();

  ASSERT_THAT(syscall(SYS_write, cleanup.fd, HELLO, HELLO_SIZE),
              Succeeds(HELLO_SIZE));
  ASSERT_THAT(syscall(SYS_lseek, cleanup.fd, 0, SEEK_SET), Succeeds(0l));

  char read_buf[HELLO_SIZE] = {};
  ASSERT_THAT(syscall(SYS_read, cleanup.fd, read_buf, HELLO_SIZE),
              Succeeds(HELLO_SIZE));
  EXPECT_STREQ(read_buf, HELLO);

  ASSERT_THAT(syscall(SYS_close, cleanup.fd), Succeeds(0l));
  cleanup.fd = -1;
}
