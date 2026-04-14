//===-- Windows unittests for mkfifo --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Windows named pipes live in \Device\NamedPipe\, not the filesystem.
// mkfifo cannot create a POSIX FIFO node on Windows and must return ENOTSUP.
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/mkfifo.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LlvmLibcWindowsMkfifoTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// mkfifo must always fail with ENOTSUP on Windows.
TEST_F(LlvmLibcWindowsMkfifoTest, AlwaysEnotsup) {
  EXPECT_THAT(LIBC_NAMESPACE::mkfifo("test.fifo", S_IRWXU), Fails(ENOTSUP));
}
