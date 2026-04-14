//===-- Implementation of inet_ntoa function ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/arpa/inet/inet_ntoa.h"

#include "include/llvm-libc-macros/arpa-inet-macros.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/common.h"
#include "src/arpa/inet/inet_ntop.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, inet_ntoa, (in_addr in)) {
  static thread_local char buffer[INET_ADDRSTRLEN];
  return const_cast<char *>(inet_ntop(AF_INET, &in, buffer, INET_ADDRSTRLEN));
}

} // namespace LIBC_NAMESPACE_DECL
