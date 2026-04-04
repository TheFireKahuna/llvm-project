//===-- Windows tests for slashless sem_open names ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/semaphore/sem_close.h"
#include "src/semaphore/sem_open.h"
#include "src/semaphore/sem_post.h"
#include "src/semaphore/sem_trywait.h"
#include "src/stdio/snprintf.h"
#include "src/unistd/getpid.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

namespace {

using ::init_object_attributes;
using ::to_base_named_object_path;

class LlvmLibcWindowsSemOpenTest
    : public LIBC_NAMESPACE::testing::ErrnoCheckingTest {
protected:
  char LeafName[96] = {};
  HANDLE DirectHandle = nullptr;

  void SetUp() override {
    LIBC_NAMESPACE::testing::ErrnoCheckingTest::SetUp();
    LIBC_NAMESPACE::snprintf(
        LeafName, sizeof(LeafName), "\\llvm_sem_native_%ld_%p",
        static_cast<long>(LIBC_NAMESPACE::getpid()),
        static_cast<void *>(this));
  }

  void TearDown() override {
    if (DirectHandle != nullptr) {
      ::NtClose(DirectHandle);
      DirectHandle = nullptr;
    }
    LIBC_NAMESPACE::testing::ErrnoCheckingTest::TearDown();
  }

  void create_native_semaphore(long initial_count, long maximum_count) {
    WCHAR nt_name[MAX_NT_PATH_WCHARS];
    size_t nt_name_len =
        to_base_named_object_path(LeafName, nt_name, MAX_NT_PATH_WCHARS);
    ASSERT_NE(nt_name_len, size_t(0));

    LIBC_NAMESPACE::windows::nt_wstring_view name(nt_name, nt_name_len);
    OBJECT_ATTRIBUTES oa;
    init_object_attributes(&oa, &name);

    NTSTATUS status = ::NtCreateSemaphore(&DirectHandle, SEMAPHORE_ALL_ACCESS,
                                          &oa, initial_count, maximum_count);
    ASSERT_TRUE(NT_SUCCESS(status));
  }
};

TEST_F(LlvmLibcWindowsSemOpenTest, OpensExistingNativeLeafName) {
  create_native_semaphore(1, 1);

  sem_t *sem = LIBC_NAMESPACE::sem_open(LeafName, 0);
  ASSERT_ERRNO_SUCCESS();
  ASSERT_NE(sem, static_cast<sem_t *>(nullptr));

  EXPECT_THAT(LIBC_NAMESPACE::sem_trywait(sem), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::sem_trywait(sem), Fails(EAGAIN));
  EXPECT_THAT(LIBC_NAMESPACE::sem_post(sem), Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::sem_close(sem), Succeeds());
}

TEST_F(LlvmLibcWindowsSemOpenTest, MissingNativeLeafNameFailsWithEnoent) {
  sem_t *sem = LIBC_NAMESPACE::sem_open(LeafName, 0);
  EXPECT_EQ(sem, static_cast<sem_t *>(nullptr));
  ASSERT_ERRNO_EQ(ENOENT);
}

TEST_F(LlvmLibcWindowsSemOpenTest, NativeLeafNamesRejectCreateFlags) {
  sem_t *sem = LIBC_NAMESPACE::sem_open(LeafName, O_CREAT, 0600, 1);
  EXPECT_EQ(sem, static_cast<sem_t *>(nullptr));
  ASSERT_ERRNO_EQ(EINVAL);
}

} // namespace
