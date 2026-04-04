//===-- Windows implementation of arc4random -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/arc4random.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/unistd/getentropy.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(uint32_t, arc4random, (void)) {
  uint32_t val;
  LIBC_NAMESPACE::getentropy(&val, sizeof(val));
  return val;
}

} // namespace LIBC_NAMESPACE_DECL
