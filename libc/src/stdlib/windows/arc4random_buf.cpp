//===-- Windows implementation of arc4random_buf ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/arc4random_buf.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/unistd/getentropy.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, arc4random_buf, (void *buf, size_t nbytes)) {
  // getentropy is limited to 256 bytes per call.
  auto *p = static_cast<unsigned char *>(buf);
  while (nbytes > 0) {
    size_t chunk = nbytes > 256 ? 256 : nbytes;
    LIBC_NAMESPACE::getentropy(p, chunk);
    p += chunk;
    nbytes -= chunk;
  }
}

} // namespace LIBC_NAMESPACE_DECL
