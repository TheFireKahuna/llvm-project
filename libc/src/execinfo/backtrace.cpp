//===-- Implementation of backtrace (stub) --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "backtrace.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, backtrace,
                   ([[maybe_unused]] void **buffer,
                    [[maybe_unused]] int size)) {
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
