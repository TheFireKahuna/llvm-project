//===-- entry_main.cpp - Console application entry point ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT entry point for console applications using narrow (char) main().
// Command line and environment initialization delegates to UCRT.
//
// Entry points are split into separate translation units so the linker only
// pulls in the entry point actually used by the application.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "internal.h"
#include <stdlib.h> // exit()

// main has C++ linkage per standard, declared here to allow calling from CRT.
int main(int, char **, char **);

extern "C" void __cdecl mainCRTStartup(void) {
  _set_app_type(_crt_console_app);
  if (_configure_narrow_argv(_crt_argv_unexpanded_arguments) != 0)
    wincrt::fatalError(wincrt::RuntimeError::SpaceArg);
  if (_initialize_narrow_environment() != 0)
    wincrt::fatalError(wincrt::RuntimeError::SpaceEnv);
  wincrt::commonInit();
  exit(main(wincrt::argc(), wincrt::argv(), wincrt::environ()));
}

#endif // LLVM_RUNTIME_WIN32
