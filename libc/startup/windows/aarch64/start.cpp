//===-- Implementation of mainCRTStartup for AArch64 Windows --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/attributes.h"
#include "startup/windows/do_start.h"

// Entry point for console applications.
// The Windows loader calls this function when the executable starts.
extern "C" [[noreturn]] void mainCRTStartup() {
  __libc_do_start();
}

// Entry point for GUI applications (WinMain).
extern "C" [[noreturn]] void WinMainCRTStartup() {
  __libc_do_start();
}
