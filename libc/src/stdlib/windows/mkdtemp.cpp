//===-- Windows implementation of mkdtemp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/mkdtemp.h"
#include "src/__support/OSUtil/windows/io/mkdtemp_ops.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, mkdtemp, (char *tmpl)) {
  intptr_t ret = internal::mkdtemp(tmpl);
  if (ret < 0) {
    libc_errno = static_cast<int>(-ret);
    return nullptr;
  }
  return tmpl;
}

} // namespace LIBC_NAMESPACE_DECL
