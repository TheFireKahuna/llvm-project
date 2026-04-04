//===-- Windows implementation of creat ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/fcntl/creat.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/open.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, creat, (const char *path, int mode_flags)) {
  auto result = windows_syscalls::open(path, O_CREAT | O_WRONLY | O_TRUNC,
                                       static_cast<mode_t>(mode_flags));
  if (!result.has_value()) {
    libc_errno = result.error();
    return -1;
  }
  return result.value();
}

} // namespace LIBC_NAMESPACE_DECL
