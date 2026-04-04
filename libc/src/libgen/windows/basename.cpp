//===-- Windows implementation of basename -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/libgen/basename.h"

#include "src/__support/OSUtil/windows/io/libgen_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, basename, (char *path)) {
  return internal::basename(path);
}

} // namespace LIBC_NAMESPACE_DECL
