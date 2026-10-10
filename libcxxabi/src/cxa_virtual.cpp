//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "cxxabi.h"
#include "abort_message.h"

#ifdef _WIN32_ITANIUM
#  include <stdlib.h>
#endif

namespace __cxxabiv1 {
extern "C" {
[[noreturn]] _LIBCXXABI_FUNC_VIS
void __cxa_pure_virtual(void) {
#ifdef _WIN32_ITANIUM
  // A program installs its handler for pure virtual calls in the C runtime
  // with _set_purecall_handler. The vtables clang builds for this target
  // name _purecall, which runs it; older objects' vtables and direct calls
  // still reach this entry point, which runs it too.
  if (_purecall_handler handler = _get_purecall_handler())
    handler();
#endif
  __abort_message("Pure virtual function called!");
}

[[noreturn]] _LIBCXXABI_FUNC_VIS
void __cxa_deleted_virtual(void) {
  __abort_message("Deleted virtual function called!");
}
} // extern "C"
} // abi
