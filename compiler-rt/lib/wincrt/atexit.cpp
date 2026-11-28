//===-- atexit.cpp - atexit, _onexit and at_quick_exit --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// vcruntime defines these; the Universal CRT exports only the table
// primitives. Registering with __cxa_atexit's registry keeps C functions in
// one order with C++ static destructors. They stay in each image, since each
// passes its own image's __dso_handle, so that the image's unloading runs what
// it registered. The adaptors carry the salted destructor type, as clang's
// destructors do.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

using wincrt::Destructor;

static void callOnexit(void *Function) WINCRT_DTOR_SALT {
  (void)reinterpret_cast<_onexit_t>(Function)();
}

extern "C" {

int __cdecl atexit(void(__cdecl *Function)(void)) {
  if (!Function)
    return -1;
  return __wincrt_atexit(reinterpret_cast<Destructor>(wincrt::callVoid),
                         reinterpret_cast<void *>(Function), &__dso_handle);
}

_onexit_t __cdecl _onexit(_onexit_t Function) {
  if (!Function ||
      __wincrt_atexit(reinterpret_cast<Destructor>(callOnexit),
                      reinterpret_cast<void *>(Function), &__dso_handle))
    return nullptr;
  return Function;
}

// The traditional name. The Universal CRT does not export _onexit, so no
// import library can map this name to it. Weak, so that a program's own
// onexit takes precedence.
__attribute__((weak)) _onexit_t __cdecl onexit(_onexit_t Function) {
  return _onexit(Function);
}

int __cdecl at_quick_exit(void(__cdecl *Function)(void)) {
  return __cxa_at_quick_exit(Function, &__dso_handle);
}

} // extern "C"
