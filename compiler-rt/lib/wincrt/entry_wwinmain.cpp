//===-- entry_wwinmain.cpp - The entry point of a wide GUI program --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "entry.h"

static int invokeMain() {
  return wWinMain(reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr,
                  _get_wide_winmain_command_line(), wincrt::showWindowMode());
}

extern "C" void wWinMainCRTStartup() {
  wincrt::runExecutable<_crt_gui_app, true, invokeMain>();
}
