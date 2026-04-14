//===-- Implementation of posix_spawnattr_getschedpolicy -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "posix_spawnattr_getschedpolicy.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, posix_spawnattr_getschedpolicy,
                    (const posix_spawnattr_t *__restrict attr,
                     int *__restrict policy)) {
  *policy = attr->__schedpolicy;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
