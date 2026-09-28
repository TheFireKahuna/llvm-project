//===-- atexit.cpp - C registration functions -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// vcruntime defines these; the Universal CRT exports only the table
// primitives. Registering through __cxa_atexit keeps C functions in one order
// with C++ static destructors, and ties each to the image that registered it,
// so that the image's unloading runs it.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

using wincrt::Destructor;

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
void callFunction(void *Function) WINCRT_DTOR_SALT {
  reinterpret_cast<void(__cdecl *)(void)>(Function)();
}

void callOnexit(void *Function) WINCRT_DTOR_SALT {
  (void)reinterpret_cast<_onexit_t>(Function)();
}
#pragma clang diagnostic pop

} // namespace

extern "C" {

int __cdecl atexit(void(__cdecl *Function)(void)) {
  if (!Function)
    return -1;
  return __cxa_atexit(reinterpret_cast<Destructor>(callFunction),
                      reinterpret_cast<void *>(Function), &__dso_handle);
}

_onexit_t __cdecl _onexit(_onexit_t Function) {
  if (!Function ||
      __cxa_atexit(reinterpret_cast<Destructor>(callOnexit),
                   reinterpret_cast<void *>(Function), &__dso_handle))
    return nullptr;
  return Function;
}

int __cdecl at_quick_exit(void(__cdecl *Function)(void)) {
  return __cxa_at_quick_exit(Function, &__dso_handle);
}

} // extern "C"
