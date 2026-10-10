//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// Starting a thread reads the app model's thread initialization policy
// without loading kernel.appcore.dll or msvcrt.dll, which the Universal CRT's
// _beginthreadex loads for it. The Universal CRT's per-thread state still
// works on the thread, and outside a packaged app nothing loads combase.dll
// to initialize the Windows Runtime on it.

#include <cassert>
#include <cerrno>
#include <cstring>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main(int, char**) {
  int threadErrno = 0;
  char* threadToken = nullptr;
  std::thread t([&] {
    errno = 1234;
    threadErrno = errno;
    char text[] = "a b";
    char* context = nullptr;
    threadToken = strtok_s(text, " ", &context) == text ? text : nullptr;
  });
  t.join();
  assert(threadErrno == 1234);
  assert(threadToken != nullptr);
  assert(errno != 1234);
  assert(GetModuleHandleW(L"kernel.appcore.dll") == nullptr);
  assert(GetModuleHandleW(L"msvcrt.dll") == nullptr);
  assert(GetModuleHandleW(L"combase.dll") == nullptr);
  return 0;
}
