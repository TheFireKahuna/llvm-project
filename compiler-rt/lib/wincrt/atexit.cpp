//===-- atexit.cpp - C registration functions over the Itanium registry ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// vcruntime provides these statically; UCRT exports only the table primitives
// (_crt_atexit, _crt_at_quick_exit). Registering through __cxa_atexit keeps C
// callbacks in reverse registration order with C++ static destructors and
// tags them with the calling image so DLL unload removes them.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

namespace {

void __cdecl callVoid(void *Function) {
  reinterpret_cast<void (*)(void)>(Function)();
}

void __cdecl callOnexit(void *Function) {
  (void)reinterpret_cast<_onexit_t>(Function)();
}

} // namespace

extern "C" {

int __cdecl atexit(void(__cdecl *Function)(void)) {
  if (!Function)
    return -1;
  return __cxa_atexit(callVoid, reinterpret_cast<void *>(Function),
                      __dso_handle);
}

int __cdecl at_quick_exit(void(__cdecl *Function)(void)) {
  if (!Function)
    return -1;
  return __cxa_at_quick_exit(Function, __dso_handle);
}

_onexit_t __cdecl _onexit(_onexit_t Function) {
  if (!Function || __cxa_atexit(callOnexit, reinterpret_cast<void *>(Function),
                                __dso_handle) != 0)
    return nullptr;
  return Function;
}

// Legacy MSVC DLL registration. The table pointers described a per-DLL
// onexit table in older CRTs; the DSO token provides the same scoping.
_onexit_t __cdecl __dllonexit(_onexit_t Function, _PVFV **, _PVFV **) {
  return _onexit(Function);
}

} // extern "C"
