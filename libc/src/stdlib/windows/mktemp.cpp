//===-- Windows implementation of mktemp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/mktemp.h"
#include "src/__support/OSUtil/windows/io/mktemp_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, mktemp, (char *tmpl)) {
  int ret = internal::mktemp(tmpl);
  if (ret < 0) {
    libc_errno = -ret;
    tmpl[0] = '\0';
    return tmpl;
  }
  return tmpl;
}

} // namespace LIBC_NAMESPACE_DECL
