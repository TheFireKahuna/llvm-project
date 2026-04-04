//===-- Implementation of backtrace_symbols_fd (stub) ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "backtrace_symbols_fd.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, backtrace_symbols_fd,
                   ([[maybe_unused]] void *const *buffer,
                    [[maybe_unused]] int size,
                    [[maybe_unused]] int fd)) {
  // Stub — no output.
}

} // namespace LIBC_NAMESPACE_DECL
