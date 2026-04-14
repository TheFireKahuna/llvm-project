//===-- Windows implementation of ptsname_r -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/ptsname_r.h"

#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, ptsname_r, (int fd, char *buffer, size_t size)) {
  return internal::vt_pty::ptsname_r(fd, buffer, size);
}

} // namespace LIBC_NAMESPACE_DECL
