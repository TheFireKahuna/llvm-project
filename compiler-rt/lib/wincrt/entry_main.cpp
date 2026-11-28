//===-- entry_main.cpp - The entry point of a console program -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "entry.h"

int main(int, char **, char **);

static int invokeMain() {
  return main(*__p___argc(), *__p___argv(), *__p__environ());
}

extern "C" void mainCRTStartup() {
  wincrt::runExecutable<_crt_console_app, false, invokeMain>();
}
