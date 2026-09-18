//===-- entry_main.cpp - Console entry point for main() -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each entry point is its own translation unit so the linker extracts only
// the one the program's /entry names, and only that one references its user
// main function.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

int main(int, char **, char **);

namespace {
int invokeMain() { return main(*__p___argc(), *__p___argv(), *__p__environ()); }
} // namespace

extern "C" void __cdecl mainCRTStartup(void) {
  // The cookie must be live before any frame in this image is protected.
  __security_init_cookie();
  wincrt::runExecutable(_crt_console_app, _configure_narrow_argv,
                        _initialize_narrow_environment, invokeMain);
}
