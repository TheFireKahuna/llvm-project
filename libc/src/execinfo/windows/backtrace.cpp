//===-- Windows implementation of backtrace -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/execinfo/backtrace.h"

#include "src/__support/OSUtil/windows/debug/backtrace.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, backtrace, (void **buffer, int size)) {
  return internal::posix_backtrace(buffer, size);
}

} // namespace LIBC_NAMESPACE_DECL
