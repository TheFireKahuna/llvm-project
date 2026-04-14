//===-- Windows NTPOSIX implementation of setenv --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/setenv.h"
#include "src/__support/OSUtil/windows/io/env_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, setenv,
                   (const char *name, const char *value, int overwrite)) {
  return internal::env_set(name, value, overwrite);
}

} // namespace LIBC_NAMESPACE_DECL
