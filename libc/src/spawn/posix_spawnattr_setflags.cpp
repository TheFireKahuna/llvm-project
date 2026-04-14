//===-- Implementation of posix_spawnattr_setflags ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "posix_spawnattr_setflags.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, posix_spawnattr_setflags,
                    (posix_spawnattr_t * attr, short flags)) {
  attr->__flags = flags;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
