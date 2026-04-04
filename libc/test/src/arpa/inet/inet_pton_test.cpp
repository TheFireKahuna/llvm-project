//===-- Unittests for inet_pton -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/libc_errno.h"
#include "src/arpa/inet/inet_pton.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"

namespace LIBC_NAMESPACE_DECL {

using LlvmLibcInetPton = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcInetPton, ParsesStrictIpv4Address) {
  uint8_t out[4] = {};
  ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", out));
  ASSERT_EQ(uint8_t(127), out[0]);
  ASSERT_EQ(uint8_t(0), out[1]);
  ASSERT_EQ(uint8_t(0), out[2]);
  ASSERT_EQ(uint8_t(1), out[3]);
}

TEST_F(LlvmLibcInetPton, RejectsLegacyIpv4Notation) {
  uint8_t out[4] = {};
  ASSERT_EQ(0, inet_pton(AF_INET, "1.2.03.4", out));
  ASSERT_EQ(0, inet_pton(AF_INET, "1.2.0xabcd", out));
}

TEST_F(LlvmLibcInetPton, ParsesIpv6Addresses) {
  uint8_t out[16] = {};
  ASSERT_EQ(1, inet_pton(AF_INET6, "::1", out));
  for (int i = 0; i < 15; ++i)
    ASSERT_EQ(uint8_t(0), out[i]);
  ASSERT_EQ(uint8_t(1), out[15]);

  constexpr uint8_t expected[16] = {0, 1, 0, 2, 0, 3, 0, 4,
                                    0, 5, 0, 6, 0, 7, 0, 0};
  ASSERT_EQ(1, inet_pton(AF_INET6, "1:2:3:4:5:6:7::", out));
  for (int i = 0; i < 16; ++i)
    ASSERT_EQ(expected[i], out[i]);
}

TEST_F(LlvmLibcInetPton, UnsupportedFamilyFails) {
  uint8_t out[16] = {};
  ASSERT_EQ(-1, inet_pton(12345, "", out));
  ASSERT_ERRNO_EQ(EAFNOSUPPORT);
}

} // namespace LIBC_NAMESPACE_DECL
