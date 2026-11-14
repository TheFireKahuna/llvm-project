//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium

// __cxa_pure_virtual runs the handler that a program sets with the Universal
// CRT's _set_purecall_handler, as _purecall does, which Clang's vtables name.

#include <cxxabi.h>
#include <stdlib.h>

static void handler() { _exit(0); }

int main(int, char**) {
  _set_purecall_handler(handler);
  abi::__cxa_pure_virtual();
}
