//===-- entry_wmain.cpp - Wide console application entry point ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT entry point for console applications using wide (wchar_t) wmain().
// Command line and environment initialization delegates to UCRT.
//
// Entry points are split into separate translation units so the linker only
// pulls in the entry point actually used by the application.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "internal.h"
#include <stdlib.h> // exit()

int wmain(int, wchar_t **, wchar_t **);

extern "C" void __cdecl wmainCRTStartup(void) {
  _set_app_type(_crt_console_app);
  if (_configure_wide_argv(_crt_argv_unexpanded_arguments) != 0)
    wincrt::fatalError(wincrt::RuntimeError::SpaceArg);
  if (_initialize_wide_environment() != 0)
    wincrt::fatalError(wincrt::RuntimeError::SpaceEnv);
  wincrt::commonInit();
  exit(wmain(wincrt::argc(), wincrt::wargv(), wincrt::wenviron()));
}

#endif // _WIN32
