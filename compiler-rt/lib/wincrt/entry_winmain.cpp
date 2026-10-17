//===-- entry_winmain.cpp - Entry point of a WinMain() program ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

WINCRT_WRAP_UCRT

// A GUI program that defines main rather than WinMain gets this entry point
// too, from the subsystem, and runs its main as a GUI application, as with
// MinGW.
extern "C" int WINAPI __wincrt_WinMain(HINSTANCE, HINSTANCE, LPSTR, int);
WINCRT_ALTERNATENAME(WinMain, __wincrt_WinMain)

static int invokeMain() {
  return WinMain(reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr,
                 _get_narrow_winmain_command_line(), wincrt::showWindowMode());
}

extern "C" void __cdecl WinMainCRTStartup(void) {
  __security_init_cookie();
  wincrt::runExecutable<_crt_gui_app, false, invokeMain>();
}
