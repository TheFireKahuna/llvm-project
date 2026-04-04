//===-- Windows implementation of sigaltstack ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin entry point — delegates to signal_state::sigaltstack() in
// signal_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "src/signal/sigaltstack.h"

#include "src/__support/OSUtil/windows/syscall_wrappers/sigaltstack.h"
#include "hdr/types/stack_t.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// ---------------------------------------------------------------------------
// POSIX entry point — libc/kernel ABI bridge.
// ---------------------------------------------------------------------------
LLVM_LIBC_FUNCTION(int, sigaltstack,
                   (const stack_t *__restrict ss, stack_t *__restrict oss)) {
  auto result = windows_syscalls::sigaltstack(ss, oss);
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
