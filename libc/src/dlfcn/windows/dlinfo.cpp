//===-- Windows implementation of dlinfo ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/dlfcn/dlinfo.h"

#include "error_state.h"
#include "src/__support/OSUtil/windows/dlfcn/dlfcn_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, dlinfo,
                   (void *__restrict handle, int request,
                    void *__restrict info)) {
  intptr_t ret = internal::dlinfo(handle, request, info);
  if (ret < 0) {
    dlfcn_state::set_error_from_errno(-static_cast<int>(ret));
    return -1;
  }
  dlfcn_state::clear_error();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
