//===-- Unittests for waitid ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/wait/waitid.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include <sys/wait.h>

using namespace LIBC_NAMESPACE::testing::ErrnoSetterMatcher;
using LlvmLibcWaitIdTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcWaitIdTest, NoChildFails) {
  siginfo_t info = {};
  ASSERT_THAT(LIBC_NAMESPACE::waitid(P_ALL, 0, &info, WEXITED | WNOHANG),
              Fails(ECHILD));
}

TEST_F(LlvmLibcWaitIdTest, InvalidOptionsFail) {
  siginfo_t info = {};
  ASSERT_THAT(LIBC_NAMESPACE::waitid(P_ALL, 0, &info, 0), Fails(EINVAL));
}
