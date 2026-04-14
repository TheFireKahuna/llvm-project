//===-- Windows unittests for sysconf -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - _SC_PAGESIZE: positive, power of 2
//   - _SC_NPROCESSORS_ONLN: at least 1
//   - _SC_PHYS_PAGES: at least 1
//   - _SC_CLK_TCK: exactly 100 (documented constant in this impl)
//   - _SC_VERSION: 200809L
//   - _SC_2_* queries mirror the corresponding _POSIX2_* macros
//   - _SC_V7_* queries reflect the target ABI honestly (ILP32_OFFBIG only on
//     32-bit targets; no V7 environment on LLP64 targets)
//   - _SC_OPEN_MAX: positive
//   - unknown name: returns -1 and sets EINVAL
//
//===----------------------------------------------------------------------===//

#include "src/unistd/sysconf.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/unistd_macros.h"

#if __SIZEOF_LONG__ == 4 && __SIZEOF_POINTER__ == 4
static_assert(_POSIX_V7_ILP32_OFFBIG == 200809L);
#else
static_assert(_POSIX_V7_ILP32_OFFBIG == -1);
#endif

#if __SIZEOF_LONG__ == 8 && __SIZEOF_POINTER__ == 8
static_assert(_POSIX_V7_LP64_OFF64 == 200809L);
#else
static_assert(_POSIX_V7_LP64_OFF64 == -1);
#endif

static_assert(_POSIX_V7_LPBIG_OFFBIG == -1);

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LlvmLibcWindowsSysconfTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcWindowsSysconfTest, PageSize) {
  long ps = LIBC_NAMESPACE::sysconf(_SC_PAGESIZE);
  EXPECT_GT(ps, 0L);
  EXPECT_EQ(ps & (ps - 1), 0L); // power of 2
}

TEST_F(LlvmLibcWindowsSysconfTest, NprocessorsOnln) {
  long n = LIBC_NAMESPACE::sysconf(_SC_NPROCESSORS_ONLN);
  EXPECT_GE(n, 1L);
}

TEST_F(LlvmLibcWindowsSysconfTest, PhysPages) {
  long pages = LIBC_NAMESPACE::sysconf(_SC_PHYS_PAGES);
  EXPECT_GT(pages, 0L);
}

TEST_F(LlvmLibcWindowsSysconfTest, AvPhysPages) {
  long pages = LIBC_NAMESPACE::sysconf(_SC_AVPHYS_PAGES);
  EXPECT_GT(pages, 0L);
}

TEST_F(LlvmLibcWindowsSysconfTest, ClkTck) {
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_CLK_TCK), 100L);
}

TEST_F(LlvmLibcWindowsSysconfTest, Version) {
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_VERSION), 200809L);
}

TEST_F(LlvmLibcWindowsSysconfTest, Posix2Queries) {
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_VERSION), 200809L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_C_BIND),
            static_cast<long>(_POSIX2_C_BIND));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_C_DEV),
            static_cast<long>(_POSIX2_C_DEV));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_CHAR_TERM),
            static_cast<long>(_POSIX2_CHAR_TERM));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_FORT_DEV),
            static_cast<long>(_POSIX2_FORT_DEV));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_FORT_RUN),
            static_cast<long>(_POSIX2_FORT_RUN));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_LOCALEDEF),
            static_cast<long>(_POSIX2_LOCALEDEF));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS), static_cast<long>(_POSIX2_PBS));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS_ACCOUNTING),
            static_cast<long>(_POSIX2_PBS_ACCOUNTING));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS_CHECKPOINT),
            static_cast<long>(_POSIX2_PBS_CHECKPOINT));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS_LOCATE),
            static_cast<long>(_POSIX2_PBS_LOCATE));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS_MESSAGE),
            static_cast<long>(_POSIX2_PBS_MESSAGE));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS_TRACK),
            static_cast<long>(_POSIX2_PBS_TRACK));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_SW_DEV),
            static_cast<long>(_POSIX2_SW_DEV));
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_UPE),
            static_cast<long>(_POSIX2_UPE));
}

TEST_F(LlvmLibcWindowsSysconfTest, V7ProgrammingEnvironments) {
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_V7_ILP32_OFF32), -1L);
#if __SIZEOF_LONG__ == 4 && __SIZEOF_POINTER__ == 4
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_V7_ILP32_OFFBIG),
            static_cast<long>(_POSIX_V7_ILP32_OFFBIG));
#else
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_V7_ILP32_OFFBIG), -1L);
#endif
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_V7_LP64_OFF64), -1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_V7_LPBIG_OFFBIG), -1L);
}

TEST_F(LlvmLibcWindowsSysconfTest, OpenMax) {
  long om = LIBC_NAMESPACE::sysconf(_SC_OPEN_MAX);
  EXPECT_GT(om, 0L);
}

TEST_F(LlvmLibcWindowsSysconfTest, AdditionalStandardQueries) {
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_AIO_MAX), -1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_AIO_LISTIO_MAX), -1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_2_PBS), -1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_DEVICE_CONTROL), -1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_V8_LP64_OFF64), -1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_XOPEN_SHM), 1L);
  EXPECT_EQ(LIBC_NAMESPACE::sysconf(_SC_XOPEN_VERSION), 700L);
}

// An unrecognised name must return -1 and set EINVAL.
TEST_F(LlvmLibcWindowsSysconfTest, UnknownName) {
  EXPECT_THAT(LIBC_NAMESPACE::sysconf(99999), Fails<long>(EINVAL));
}
