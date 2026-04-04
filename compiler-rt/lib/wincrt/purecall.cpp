//===-- purecall.cpp - Pure virtual call handler --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bridges _purecall (MSVC ABI) to __cxa_pure_virtual (Itanium ABI).
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "internal.h"

// Provided by libc++abi.
extern "C" {
WINCRT_NORETURN void __cxa_pure_virtual(void);
WINCRT_NORETURN void __cxa_deleted_virtual(void);
}

namespace {

_purecall_handler g_purecallHandler = nullptr;

} // namespace

extern "C" {

_purecall_handler __cdecl _get_purecall_handler(void) {
  return __atomic_load_n(&g_purecallHandler, __ATOMIC_ACQUIRE);
}

_purecall_handler __cdecl _set_purecall_handler(_purecall_handler handler) {
  return __atomic_exchange_n(&g_purecallHandler, handler, __ATOMIC_ACQ_REL);
}

int __cdecl _purecall(void) {
  _purecall_handler handler = _get_purecall_handler();
  if (handler) {
    handler();
    abort();
  }

  __cxa_pure_virtual();
}

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
