//===-- entry_main.cpp - Entry point of a main() program ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

WINCRT_WRAP_UCRT

int main(int, char **, char **);

static int invokeMain() {
  return main(*__p___argc(), *__p___argv(), *__p__environ());
}

extern "C" void __cdecl mainCRTStartup(void) {
  // Before any frame of this image is protected by the cookie.
  __security_init_cookie();
  wincrt::runExecutable<_crt_console_app, false, invokeMain>();
}
