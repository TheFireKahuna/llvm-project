//===-- Unittests for inet_ntop -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/libc_errno.h"
#include "src/arpa/inet/inet_ntop.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/arpa-inet-macros.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"

namespace LIBC_NAMESPACE_DECL {

using LlvmLibcInetNtop = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcInetNtop, FormatsIpv4Address) {
  constexpr uint8_t src[4] = {127, 0, 0, 1};
  char buffer[INET_ADDRSTRLEN];
  ASSERT_EQ(static_cast<const char *>(buffer),
            inet_ntop(AF_INET, src, buffer, sizeof(buffer)));
  ASSERT_STREQ("127.0.0.1", buffer);
}

TEST_F(LlvmLibcInetNtop, FormatsIpv4MappedIpv6Address) {
  constexpr uint8_t src[16] = {0, 0, 0,    0,    0,   0,   0, 0,
                               0, 0, 0xff, 0xff, 192, 168, 0, 1};
  char buffer[INET6_ADDRSTRLEN];
  ASSERT_EQ(static_cast<const char *>(buffer),
            inet_ntop(AF_INET6, src, buffer, sizeof(buffer)));
  ASSERT_STREQ("::ffff:192.168.0.1", buffer);
}

TEST_F(LlvmLibcInetNtop, SmallBufferFails) {
  constexpr uint8_t src[4] = {127, 0, 0, 1};
  char buffer[1] = {};
  ASSERT_EQ(inet_ntop(AF_INET, src, buffer, sizeof(buffer)),
            static_cast<const char *>(nullptr));
  ASSERT_ERRNO_EQ(ENOSPC);
}

TEST_F(LlvmLibcInetNtop, UnsupportedFamilyFails) {
  char buffer[INET6_ADDRSTRLEN];
  ASSERT_EQ(inet_ntop(12345, buffer, buffer, sizeof(buffer)),
            static_cast<const char *>(nullptr));
  ASSERT_ERRNO_EQ(EAFNOSUPPORT);
}

} // namespace LIBC_NAMESPACE_DECL
