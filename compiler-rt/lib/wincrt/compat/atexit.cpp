//===-- atexit.cpp - atexit/_onexit delegation to __cxa_atexit ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Delegates atexit() to __cxa_atexit for strict Itanium LIFO ordering.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "../internal.h"

namespace {

void __cdecl atexitTrampoline(void *arg) {
  auto userFunc = reinterpret_cast<void (*)(void)>(arg);
  userFunc();
}

} // namespace

extern "C" int __cdecl atexit(void (*func)(void)) {
  if (!func)
    return -1;
  return __cxa_atexit(atexitTrampoline, reinterpret_cast<void *>(func),
                      nullptr);
}

extern "C" {

typedef int(__cdecl *_onexit_t)(void);

void __cdecl onexitTrampoline(void *arg) {
  auto userFunc = reinterpret_cast<_onexit_t>(arg);
  (void)userFunc();
}

_onexit_t __cdecl _onexit(_onexit_t func) {
  if (!func)
    return nullptr;
  // nullptr dso_handle: handler runs at process exit, not DLL unload.
  int result =
      __cxa_atexit(onexitTrampoline, reinterpret_cast<void *>(func), nullptr);
  return (result == 0) ? func : nullptr;
}

/// Legacy DLL onexit registration. The pbegin/pend parameters pointed to a
/// per-DLL function table in old CRT; we ignore them and use __cxa_atexit
/// with __dso_handle for proper per-DLL cleanup via Itanium ABI.
///
/// This shim exists because:
/// 1. UCRT doesn't export __dllonexit (only old msvcrt.dll does)
/// 2. Some static libraries compiled with older MSVC reference it
/// 3. Delegating to __cxa_atexit provides correct DLL unload semantics
_onexit_t __cdecl __dllonexit(_onexit_t func, _PVFV **, _PVFV **) {
  if (!func)
    return nullptr;
  // Use __dso_handle so cleanup runs on DLL unload, not just process exit.
  int result =
      __cxa_atexit(onexitTrampoline, reinterpret_cast<void *>(func), __dso_handle);
  return (result == 0) ? func : nullptr;
}

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
