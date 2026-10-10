//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium

// std::random_device draws from ProcessPrng without loading advapi32.dll,
// msvcrt.dll or the other DLLs that the Universal CRT's rand_s loads, and the
// values it returns vary.

#include <cassert>
#include <random>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main(int, char**) {
  std::random_device rd;
  unsigned first = rd();
  bool varied    = false;
  for (int i = 0; i < 16 && !varied; ++i)
    varied = rd() != first;
  assert(varied);
  assert(GetModuleHandleW(L"advapi32.dll") == nullptr);
  assert(GetModuleHandleW(L"msvcrt.dll") == nullptr);
  assert(GetModuleHandleW(L"kernel.appcore.dll") == nullptr);
  return 0;
}
