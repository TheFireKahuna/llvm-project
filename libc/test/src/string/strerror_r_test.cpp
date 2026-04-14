//===-- Unittests for strerror --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/string/strerror_r.h"
#include "test/UnitTest/Test.h"

#include <errno.h>
#include <stddef.h>

// XSI-compliant strerror_r: returns 0 on success, copies message into buf.
TEST(LlvmLibcStrErrorRTest, KnownError) {
  const size_t BUFF_SIZE = 128;
  char buffer[BUFF_SIZE];
  ASSERT_EQ(LIBC_NAMESPACE::strerror_r(0, buffer, BUFF_SIZE), 0);
  ASSERT_STREQ(buffer, "Success");
}

TEST(LlvmLibcStrErrorRTest, UnknownError) {
  const size_t BUFF_SIZE = 128;
  char buffer[BUFF_SIZE];
  ASSERT_EQ(LIBC_NAMESPACE::strerror_r(-1, buffer, BUFF_SIZE), 0);
  ASSERT_STREQ(buffer, "Unknown error -1");
}

TEST(LlvmLibcStrErrorRTest, BufferTooSmall) {
  char buffer[4];
  ASSERT_EQ(LIBC_NAMESPACE::strerror_r(0, buffer, sizeof(buffer)), ERANGE);
  // Should still NUL-terminate the truncated output.
  ASSERT_EQ(buffer[3], '\0');
}
