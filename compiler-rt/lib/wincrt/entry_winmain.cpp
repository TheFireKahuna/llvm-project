//===-- entry_winmain.cpp - GUI application entry point -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT entry point for GUI applications using narrow (char) WinMain().
// Command line and environment initialization delegates to UCRT.
//
// Entry points are split into separate translation units so the linker only
// pulls in the entry point actually used by the application.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "internal.h"
#include <stdlib.h> // exit()

extern "C" int __stdcall WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

extern "C" void __cdecl WinMainCRTStartup(void) {
  _set_app_type(_crt_gui_app);
  if (_configure_narrow_argv(_crt_argv_unexpanded_arguments) != 0)
    wincrt::fatalError(wincrt::RuntimeError::SpaceArg);
  if (_initialize_narrow_environment() != 0)
    wincrt::fatalError(wincrt::RuntimeError::SpaceEnv);
  wincrt::commonInit();

  HINSTANCE hInstance = GetModuleHandleW(nullptr);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  GetStartupInfoW(&si);
  int nCmdShow = (si.dwFlags & STARTF_USESHOWWINDOW) ? si.wShowWindow
                                                     : SW_SHOWDEFAULT;

  LPSTR cmdline = _get_narrow_winmain_command_line();
  exit(WinMain(hInstance, nullptr, cmdline, nCmdShow));
}

#endif // LLVM_RUNTIME_WIN32
