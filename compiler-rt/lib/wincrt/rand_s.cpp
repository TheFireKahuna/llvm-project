//===-- rand_s.cpp - rand_s -----------------------------------------------===//
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

// The call goes to a function of the system, which has no KCFI type.
extern "C" WINCRT_API __attribute__((no_sanitize("kcfi"))) errno_t __cdecl
__wrap_rand_s(unsigned *Result) {
  using ProcessPrngType = BOOL(WINAPI *)(PBYTE, SIZE_T);
  static ProcessPrngType ProcessPrng;
  if (!Result) {
    errno = EINVAL;
    _invalid_parameter_noinfo();
    return EINVAL;
  }
  *Result = 0;
  ProcessPrngType Draw = __atomic_load_n(&ProcessPrng, __ATOMIC_ACQUIRE);
  if (!Draw) {
    HMODULE Module = LoadLibraryExW(L"bcryptprimitives.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    Draw = reinterpret_cast<ProcessPrngType>(
        Module ? GetProcAddress(Module, "ProcessPrng") : nullptr);
    __atomic_store_n(&ProcessPrng, Draw, __ATOMIC_RELEASE);
  }
  if (!Draw || !Draw(reinterpret_cast<PBYTE>(Result), sizeof(*Result)))
    return errno = ENOMEM;
  return 0;
}
