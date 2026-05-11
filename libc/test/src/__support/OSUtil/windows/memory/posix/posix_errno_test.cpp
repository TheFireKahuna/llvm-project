//===-- POSIX-layer errno-override tests ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/posix/posix_errno.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "test/UnitTest/Test.h"

namespace mp = LIBC_NAMESPACE::windows::memory_posix;

TEST(LlvmLibcMemoryPosixErrnoTest, FixedNoreplaceMapsConflictingToEexist) {
  EXPECT_EQ(mp::errno_for_fixed_noreplace_failure(STATUS_CONFLICTING_ADDRESSES),
            EEXIST);
}

TEST(LlvmLibcMemoryPosixErrnoTest, FixedNoreplacePassesOtherStatusThrough) {
  EXPECT_EQ(mp::errno_for_fixed_noreplace_failure(STATUS_NO_MEMORY), ENOMEM);
  EXPECT_EQ(mp::errno_for_fixed_noreplace_failure(STATUS_INVALID_PARAMETER),
            EINVAL);
}

TEST(LlvmLibcMemoryPosixErrnoTest, HugetlbMapsAccessDeniedToEperm) {
  EXPECT_EQ(mp::errno_for_hugetlb_failure(STATUS_ACCESS_DENIED), EPERM);
}

TEST(LlvmLibcMemoryPosixErrnoTest, HugetlbPassesPrivilegeNotHeldAsEperm) {
  // STATUS_PRIVILEGE_NOT_HELD already maps to EPERM in the generic table,
  // so the override returns the same answer via passthrough.
  EXPECT_EQ(mp::errno_for_hugetlb_failure(STATUS_PRIVILEGE_NOT_HELD), EPERM);
}

TEST(LlvmLibcMemoryPosixErrnoTest, HugetlbPassesOtherStatusThrough) {
  EXPECT_EQ(mp::errno_for_hugetlb_failure(STATUS_NO_MEMORY), ENOMEM);
}

TEST(LlvmLibcMemoryPosixErrnoTest, MlockMapsWorkingSetQuotaToEagain) {
  EXPECT_EQ(mp::errno_for_mlock_failure(STATUS_WORKING_SET_QUOTA), EAGAIN);
}

TEST(LlvmLibcMemoryPosixErrnoTest, MlockMapsAccessDeniedToEperm) {
  EXPECT_EQ(mp::errno_for_mlock_failure(STATUS_ACCESS_DENIED), EPERM);
}

TEST(LlvmLibcMemoryPosixErrnoTest, MlockPassesOtherStatusThrough) {
  EXPECT_EQ(mp::errno_for_mlock_failure(STATUS_NO_MEMORY), ENOMEM);
  EXPECT_EQ(mp::errno_for_mlock_failure(STATUS_INVALID_PARAMETER), EINVAL);
}

TEST(LlvmLibcMemoryPosixErrnoTest, SectionTooBigMapsToEfbig) {
  // Extension landed in nt_error.h; verify the generic table now answers
  // EFBIG for the section-creation oversize NTSTATUS.
  EXPECT_EQ(LIBC_NAMESPACE::windows_util::ntstatus_to_errno(
                STATUS_SECTION_TOO_BIG),
            EFBIG);
}

TEST(LlvmLibcMemoryPosixErrnoTest, UserMappedFileMapsToEbusy) {
  EXPECT_EQ(LIBC_NAMESPACE::windows_util::ntstatus_to_errno(
                STATUS_USER_MAPPED_FILE),
            EBUSY);
}

TEST(LlvmLibcMemoryPosixErrnoTest, NegateErrnoSuccess) {
  LIBC_NAMESPACE::ErrorOr<int> ok(42);
  EXPECT_EQ(mp::negate_errno(ok), 0);
}

TEST(LlvmLibcMemoryPosixErrnoTest, NegateErrnoFailureNegates) {
  LIBC_NAMESPACE::ErrorOr<int> err(LIBC_NAMESPACE::Error(EINVAL));
  EXPECT_EQ(mp::negate_errno(err), -EINVAL);
}

TEST(LlvmLibcMemoryPosixErrnoTest, NegateErrnoFailureAlreadyNegativePreserved) {
  // Belt-and-braces: if a caller-side error already encodes a negative
  // value, the helper passes it through rather than double-negating.
  LIBC_NAMESPACE::ErrorOr<int> err(LIBC_NAMESPACE::Error(-EAGAIN));
  EXPECT_EQ(mp::negate_errno(err), -EAGAIN);
}
