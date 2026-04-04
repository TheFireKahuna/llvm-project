//===-- Windows unittests for mkdir ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX compliance points tested:
//   - mkdir creates a new directory
//   - mkdir on an existing path returns EEXIST
//   - mkdir with a nonexistent parent returns ENOENT
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/mkdir.h"
#include "src/unistd/rmdir.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include <sys/stat.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcWindowsMkdirTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// mkdir creates the directory; rmdir then removes it.
TEST_F(LlvmLibcWindowsMkdirTest, CreateAndRemove) {
  constexpr const char *DIR = "mkdir_create.tmp";
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));
  ASSERT_THAT(LIBC_NAMESPACE::rmdir(DIR), Succeeds(0));
}

// Creating the same directory twice must return EEXIST.
TEST_F(LlvmLibcWindowsMkdirTest, AlreadyExists) {
  constexpr const char *DIR = "mkdir_exists.tmp";
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Succeeds(0));
  EXPECT_THAT(LIBC_NAMESPACE::mkdir(DIR, S_IRWXU), Fails(EEXIST));
  LIBC_NAMESPACE::rmdir(DIR);
}

// mkdir with a nonexistent parent component must return ENOENT.
TEST_F(LlvmLibcWindowsMkdirTest, MissingParent) {
  EXPECT_THAT(LIBC_NAMESPACE::mkdir("no_such_parent_xyz/child", S_IRWXU),
              Fails(ENOENT));
}
