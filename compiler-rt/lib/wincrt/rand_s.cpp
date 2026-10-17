//===-- rand_s.cpp - Random numbers from the system -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// rand_s, which every image's entry object wraps with this definition. The
// Universal CRT's draws from RtlGenRandom, which advapi32.dll exports, and
// loading advapi32.dll loads msvcrt.dll, sechost.dll and rpcrt4.dll with it.
// RtlGenRandom draws from ProcessPrng, which bcryptprimitives.dll exports and
// which needs no other DLL; it is loaded on the first draw.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <errno.h>

namespace {

using ProcessPrngType = BOOL(WINAPI *)(PBYTE, SIZE_T);

ProcessPrngType processPrng() {
  static ProcessPrngType Function;
  ProcessPrngType Current = __atomic_load_n(&Function, __ATOMIC_ACQUIRE);
  if (Current)
    return Current;
  HMODULE Module = LoadLibraryExW(L"bcryptprimitives.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
  Current = reinterpret_cast<ProcessPrngType>(
      Module ? GetProcAddress(Module, "ProcessPrng") : nullptr);
  __atomic_store_n(&Function, Current, __ATOMIC_RELEASE);
  return Current;
}

} // namespace

// The call goes to a function of the system, which has no KCFI type.
extern "C" WINCRT_ATEXIT_API
    __attribute__((no_sanitize("kcfi"))) errno_t __cdecl
    __wrap_rand_s(unsigned *Result) {
  if (!Result) {
    errno = EINVAL;
    _invalid_parameter_noinfo();
    return EINVAL;
  }
  *Result = 0;
  ProcessPrngType Draw = processPrng();
  if (!Draw || !Draw(reinterpret_cast<PBYTE>(Result), sizeof(*Result))) {
    errno = ENOMEM;
    return ENOMEM;
  }
  return 0;
}
