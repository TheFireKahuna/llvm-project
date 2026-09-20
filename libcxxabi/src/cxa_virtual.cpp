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
// The C runtime owns the handler a program installs with
// _set_purecall_handler, and vtables built by this toolchain call the entry
// point that consults it. One here may come from an older object, or from a
// direct call, so honour the handler in both places. Declared rather than
// included: the header that has it is not part of this target's C library
// headers.
extern "C" {
typedef void(__cdecl* _purecall_handler)(void);
_purecall_handler __cdecl _get_purecall_handler(void);
}
#endif

namespace __cxxabiv1 {
extern "C" {
_LIBCXXABI_FUNC_VIS _LIBCXXABI_NORETURN
void __cxa_pure_virtual(void) {
#ifdef _WIN32_ITANIUM
  if (_purecall_handler handler = _get_purecall_handler())
    handler();
#endif
  __abort_message("Pure virtual function called!");
}

_LIBCXXABI_FUNC_VIS _LIBCXXABI_NORETURN
void __cxa_deleted_virtual(void) {
  __abort_message("Deleted virtual function called!");
}
} // extern "C"
} // abi
