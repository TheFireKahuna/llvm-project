//===-- entry.h - What an executable's entry points share -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each entry point of an executable is an object of its own, so that the linker
// extracts only the one the program's main function selects.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_ENTRY_H
#define COMPILER_RT_LIB_WINCRT_ENTRY_H

#include "wincrt.h"

WINCRT_WRAP_UCRT

// The process's exit-list state, which every copy of the termination
// registries finds through the locator the linker writes for it in the
// executable's MZ header. Nothing in the image names it, so the linker is
// told to keep it.
extern "C" wincrt::ExitState __wincrt_exit_state = {
    wincrt::ExitStateVersion, sizeof(wincrt::ExitState)};
#pragma comment(linker, "/include:__wincrt_exit_state")

namespace wincrt {

// The nCmdShow argument of WinMain: what the process's creator asked for.
inline int showWindowMode() {
  STARTUPINFOW Info;
  GetStartupInfoW(&Info);
  return Info.dwFlags & STARTF_USESHOWWINDOW ? Info.wShowWindow
                                             : SW_SHOWDEFAULT;
}

// The body of an executable's entry point, which instantiates it with the call
// of its own main function, so that the call is direct. It never returns, so
// no epilogue checks the cookie that it replaces.
template <_crt_app_type Type, bool Wide, int (*Main)()>
[[noreturn]] void runExecutable() {
  __security_init_cookie();
  __try {
    _set_app_type(Type);
    if (Wide ? _configure_wide_argv(_crt_argv_unexpanded_arguments) ||
                   _initialize_wide_environment()
             : _configure_narrow_argv(_crt_argv_unexpanded_arguments) ||
                   _initialize_narrow_environment())
      fatal("not enough space for the arguments or the environment");
    initializeExecutable();
    exit(Main());
  } __except (_seh_filter_exe(GetExceptionCode(), GetExceptionInformation())) {
    // The Universal CRT turns a hardware exception into its signal; one it
    // chooses to handle here ends the program with the exception code.
    _exit(static_cast<int>(GetExceptionCode()));
  }
}

} // namespace wincrt

#endif // COMPILER_RT_LIB_WINCRT_ENTRY_H
