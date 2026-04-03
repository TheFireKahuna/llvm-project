//===-- entry.cpp - Entry points for Windows Itanium ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT entry points for executables and DLLs. Command line and environment
// initialization delegates directly to UCRT—no Itanium-specific behavior.
//
// Entry points are split into separate translation units so the linker only
// includes the entry point actually needed:
//
//   entry_main.cpp     - mainCRTStartup     (console, narrow char)
//   entry_wmain.cpp    - wmainCRTStartup    (console, wide wchar_t)
//   entry_winmain.cpp  - WinMainCRTStartup  (GUI, narrow char)
//   entry_wwinmain.cpp - wWinMainCRTStartup (GUI, wide wchar_t)
//   entry_dll.cpp      - _DllMainCRTStartup (DLL)
//
// This file is intentionally empty—see the above files for implementation.
//
//===----------------------------------------------------------------------===//
