//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "cxxabi.h"
#include "abort_message.h"
#include <stdlib.h>

namespace __cxxabiv1 {
extern "C" {
[[noreturn]] _LIBCXXABI_FUNC_VIS
void __cxa_pure_virtual(void) {
#ifdef _WIN32_ITANIUM
  // Clang's vtables name _purecall there, which runs the handler a program
  // sets with _set_purecall_handler; a call that reaches this entry runs it too.
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
