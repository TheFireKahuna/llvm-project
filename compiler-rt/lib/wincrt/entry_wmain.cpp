//===-- entry_wmain.cpp - Console entry point for wmain() -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

int wmain(int, wchar_t **, wchar_t **);

namespace {
int invokeMain() {
  return wmain(*__p___argc(), *__p___wargv(), *__p__wenviron());
}
} // namespace

extern "C" void __cdecl wmainCRTStartup(void) {
  __security_init_cookie();
  wincrt::runExecutable(_crt_console_app, _configure_wide_argv,
                        _initialize_wide_environment, invokeMain);
}
