//===-- Unittests for inet_ntop -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/libc_errno.h"
#include "src/arpa/inet/inet_ntop.h"
#include "test/UnitTest/Test.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/arpa-inet-macros.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"

namespace LIBC_NAMESPACE_DECL {

TEST(LlvmLibcInetNtop, FormatsIpv4Address) {
  constexpr uint8_t src[4] = {127, 0, 0, 1};
  char buffer[INET_ADDRSTRLEN];
  ASSERT_EQ(buffer, inet_ntop(AF_INET, src, buffer, sizeof(buffer)));
  ASSERT_STREQ("127.0.0.1", buffer);
}

TEST(LlvmLibcInetNtop, FormatsIpv4MappedIpv6Address) {
  constexpr uint8_t src[16] = {0, 0, 0,    0,    0,   0,   0, 0,
                               0, 0, 0xff, 0xff, 192, 168, 0, 1};
  char buffer[INET6_ADDRSTRLEN];
  ASSERT_EQ(buffer, inet_ntop(AF_INET6, src, buffer, sizeof(buffer)));
  ASSERT_STREQ("::ffff:192.168.0.1", buffer);
}

TEST(LlvmLibcInetNtop, SmallBufferFails) {
  constexpr uint8_t src[4] = {127, 0, 0, 1};
  char buffer[1] = {};
  libc_errno = 0;
  ASSERT_EQ(nullptr, inet_ntop(AF_INET, src, buffer, sizeof(buffer)));
  ASSERT_EQ(ENOSPC, libc_errno);
}

TEST(LlvmLibcInetNtop, UnsupportedFamilyFails) {
  char buffer[INET6_ADDRSTRLEN];
  libc_errno = 0;
  ASSERT_EQ(nullptr, inet_ntop(12345, buffer, buffer, sizeof(buffer)));
  ASSERT_EQ(EAFNOSUPPORT, libc_errno);
}

} // namespace LIBC_NAMESPACE_DECL
