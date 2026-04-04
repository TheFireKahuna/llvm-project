//===-- Unittests for inet_ntoa -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/arpa/inet/htonl.h"
#include "src/arpa/inet/inet_ntoa.h"
#include "test/UnitTest/Test.h"

namespace LIBC_NAMESPACE_DECL {

TEST(LlvmLibcInetNtoa, FormatsIpv4Address) {
  in_addr addr = {htonl(0x7f000001)};
  ASSERT_STREQ("127.0.0.1", inet_ntoa(addr));
}

} // namespace LIBC_NAMESPACE_DECL
