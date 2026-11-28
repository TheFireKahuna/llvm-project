//===-- entry_winmain.cpp - The entry point of a GUI program --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "entry.h"

// A GUI program that defines main rather than WinMain gets this entry point
// too, from the subsystem, and runs its main as a GUI application, as with
// MinGW.
WINCRT_ALTERNATENAME(WinMain, __wincrt_WinMain)

static int invokeMain() {
  return WinMain(reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr,
                 _get_narrow_winmain_command_line(), wincrt::showWindowMode());
}

extern "C" void WinMainCRTStartup() {
  wincrt::runExecutable<_crt_gui_app, false, invokeMain>();
}
