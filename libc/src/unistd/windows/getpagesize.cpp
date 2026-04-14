//===-- Windows implementation of getpagesize -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/getpagesize.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, getpagesize, ()) {
  return static_cast<int>(windows::get_page_size());
}

} // namespace LIBC_NAMESPACE_DECL
