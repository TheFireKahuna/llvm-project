//===-- entry_wwinmain.cpp - GUI entry point for wWinMain() ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

namespace {
int invokeMain() {
  STARTUPINFOW Info = {};
  Info.cb = sizeof(Info);
  GetStartupInfoW(&Info);
  int Show =
      (Info.dwFlags & STARTF_USESHOWWINDOW) ? Info.wShowWindow : SW_SHOWDEFAULT;
  return wWinMain(GetModuleHandleW(nullptr), nullptr,
                  _get_wide_winmain_command_line(), Show);
}
} // namespace

extern "C" void __cdecl wWinMainCRTStartup(void) {
  __security_init_cookie();
  wincrt::runExecutable(_crt_gui_app, _configure_wide_argv,
                        _initialize_wide_environment, invokeMain);
}
