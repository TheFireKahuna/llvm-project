//===-- entry_dll.cpp - DLL entry point -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT entry point for DLLs. Calls DllMain provided by the DLL and handles
// Itanium C++ ABI cleanup on unload (__cxa_finalize, __cxa_thread_finalize).
//
// Entry points are split into separate translation units so the linker only
// pulls in the entry point actually used by the application.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "internal.h"


//===----------------------------------------------------------------------===//
// Default DllMain for DLLs that don't define one
//===----------------------------------------------------------------------===//
//
// Runtime libraries (libunwind, libc++, etc.) don't have user code and thus
// no user-defined DllMain. This default implementation allows them to link
// wincrt without providing a stub.
//
// Uses /alternatename linker directive: if user defines DllMain, theirs wins.
// If not defined, linker falls back to __wincrt_DefaultDllMain.
//
// This is the same pattern MSVC vcruntime uses (__DefaultDllMain).
//

extern "C" BOOL __stdcall __wincrt_DefaultDllMain(HINSTANCE, DWORD, LPVOID) {
  return TRUE;
}

WINCRT_ALTERNATENAME(DllMain, __wincrt_DefaultDllMain)

extern "C" BOOL __stdcall DllMain(HINSTANCE, DWORD, LPVOID);

// Set to 1 to receive DLL_THREAD_ATTACH/DETACH in DllMain.
extern "C" __declspec(selectany) int _wincrt_enable_thread_notifications = 0;

extern "C" __declspec(dllexport) BOOL __stdcall
_DllMainCRTStartup(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  if (fdwReason == DLL_PROCESS_ATTACH) {
    wincrt::commonInit();
    BOOL result = DllMain(hinstDLL, fdwReason, lpvReserved);
    if (result && !_wincrt_enable_thread_notifications) {
      (void)DisableThreadLibraryCalls(hinstDLL);
    }
    return result;
  }

  if (fdwReason == DLL_PROCESS_DETACH) {
    if (lpvReserved == nullptr) {
      __cxa_thread_finalize_dso_unload(__dso_handle);
      __cxa_finalize(__dso_handle);
      wincrt::runPreterminators();
      wincrt::runTerminators();
    }
  }

  return DllMain(hinstDLL, fdwReason, lpvReserved);
}

#endif // LLVM_RUNTIME_WIN32
