//===-- entry_wwinmain.cpp - Entry point of a wWinMain() program ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

static int invokeMain() {
  return wWinMain(reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr,
                  _get_wide_winmain_command_line(), wincrt::showWindowMode());
}

extern "C" void __cdecl wWinMainCRTStartup(void) {
  __security_init_cookie();
  wincrt::runExecutable<_crt_gui_app, true, invokeMain>();
}
