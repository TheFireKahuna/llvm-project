//===-- crt_windows.cpp - CRT entry points for Windows --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT entry points for Windows executables and DLLs.
//
// This file provides CRT startup routines for Windows targets that don't use
// msvcrt.lib or vcruntime.lib, such as Windows Itanium with Itanium C++ ABI.
//
// DESIGN
// ======
// - Entry points (mainCRTStartup, etc.) delegate to modular implementation
// - atexit() delegates to UCRT - we don't reimplement it
// - exit() calls __cxa_finalize for C++ destructors, then delegates to UCRT
// - Command line parsing uses kernel32 directly
// - TLS support via _tls_used IMAGE_TLS_DIRECTORY structure
//
// DEPENDENCIES
// ============
// - kernel32.dll: GetCommandLineA/W, HeapAlloc, TLS support
// - ucrt (ucrtbase.dll): abort, _exit, atexit (we delegate rather than reimplement)
// - libc++abi (optional): __cxa_atexit, __cxa_finalize for C++ static destructors
//
// MINIMUM REQUIREMENTS
// ====================
// - Windows Vista (NT 6.0) or later
// - GetTickCount64() for system uptime
//
// Each entry point is guarded by a CRT_HAS_* macro so they can be compiled
// into separate object files. The linker pulls in the appropriate one based
// on which user entry point (main, wmain, WinMain, wWinMain) is defined.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "crt_windows_internal.h"

//===----------------------------------------------------------------------===//
// Entry points for executables
//===----------------------------------------------------------------------===//

#ifdef CRT_HAS_MAIN
// int main(int argc, char **argv, char **envp)
extern "C" int main(int, char**, char**);

extern "C" void __cdecl mainCRTStartup(void) {
  crt::initArgsA();
  crt::initEnvironA();
  crt::commonInit();
  exit(main(crt::getArgc(), crt::getArgv(), crt::getEnviron()));
}
#endif // CRT_HAS_MAIN

#ifdef CRT_HAS_WMAIN
// int wmain(int argc, wchar_t **argv, wchar_t **envp)
extern "C" int wmain(int, wchar_t**, wchar_t**);

extern "C" void __cdecl wmainCRTStartup(void) {
  crt::initArgsW();
  crt::initEnvironW();
  crt::commonInit();
  exit(wmain(crt::getArgc(), crt::getWargv(), crt::getWenviron()));
}
#endif // CRT_HAS_WMAIN

#ifdef CRT_HAS_WINMAIN
// int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
//                    LPSTR lpCmdLine, int nShowCmd)
extern "C" int __stdcall WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

extern "C" void __cdecl WinMainCRTStartup(void) {
  // Initialize argc/argv/environ for compatibility - some code queries these
  // globals even from GUI applications.
  crt::initArgsA();
  crt::initEnvironA();
  crt::commonInit();

  HINSTANCE hInstance = GetModuleHandleW(nullptr);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  GetStartupInfoW(&si);
  int nCmdShow = (si.dwFlags & crt::kStartfUseshowwindow) ? si.wShowWindow
                                                           : crt::kSwShowdefault;

  LPSTR cmdline = crt::getWinMainCmdLineA();
  exit(WinMain(hInstance, nullptr, cmdline, nCmdShow));
}
#endif // CRT_HAS_WINMAIN

#ifdef CRT_HAS_WWINMAIN
// int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
//                     LPWSTR lpCmdLine, int nShowCmd)
extern "C" int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int);

extern "C" void __cdecl wWinMainCRTStartup(void) {
  // Initialize argc/argv/environ for compatibility
  crt::initArgsW();
  crt::initEnvironW();
  crt::commonInit();

  HINSTANCE hInstance = GetModuleHandleW(nullptr);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  GetStartupInfoW(&si);
  int nCmdShow = (si.dwFlags & crt::kStartfUseshowwindow) ? si.wShowWindow
                                                           : crt::kSwShowdefault;

  LPWSTR cmdline = crt::getWinMainCmdLineW();
  exit(wWinMain(hInstance, nullptr, cmdline, nCmdShow));
}
#endif // CRT_HAS_WWINMAIN

//===----------------------------------------------------------------------===//
// Entry point for DLLs
//===----------------------------------------------------------------------===//

#ifdef CRT_HAS_DLLMAIN
// BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
extern "C" BOOL __stdcall DllMain(HINSTANCE, DWORD, LPVOID);

//===----------------------------------------------------------------------===//
// Thread notification control
//
// By default, we call DisableThreadLibraryCalls() after DLL_PROCESS_ATTACH
// to improve performance. Most DLLs don't need DLL_THREAD_ATTACH/DETACH
// notifications, and disabling them reduces thread creation overhead.
//
// To receive thread notifications in your DLL, define this symbol with value 1
// before linking:
//
//   extern "C" int _crt_enable_thread_notifications = 1;
//
// Or use a .def file:
//
//   _crt_enable_thread_notifications DATA
//
// This is a non-standard extension specific to this CRT implementation.
// MSVC's CRT does not disable thread notifications by default.
//===----------------------------------------------------------------------===//
extern "C" __declspec(selectany) int _crt_enable_thread_notifications = 0;

//===----------------------------------------------------------------------===//
// _DllMainCRTStartup - DLL entry point
//
// This function is called by the Windows loader for DLL attach/detach events.
// It must be exported so the PE loader can find it as the entry point.
//
// THREAD SAFETY NOTE (DLL_PROCESS_DETACH):
// When lpvReserved == nullptr (FreeLibrary call), we run cleanup handlers.
// However, Windows does not guarantee that other threads have stopped
// executing code in the DLL. This is a fundamental Windows limitation:
// - The loader holds the loader lock during DllMain
// - Other threads may be executing DLL code outside DllMain
// - There is no safe way to synchronize DLL unload with all threads
//
// Callers using FreeLibrary should ensure their own synchronization to
// prevent use-after-free. This matches MSVC CRT behavior.
//
// Reference: https://docs.microsoft.com/en-us/windows/win32/dlls/dllmain
//===----------------------------------------------------------------------===//
extern "C" __declspec(dllexport) BOOL __stdcall
_DllMainCRTStartup(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  if (fdwReason == crt::kDllProcessAttach) {
    crt::commonInit();
    BOOL result = DllMain(hinstDLL, fdwReason, lpvReserved);
    if (result && !_crt_enable_thread_notifications) {
      // Optimization: most DLLs don't need thread attach/detach notifications.
      DisableThreadLibraryCalls(hinstDLL);
    }
    return result;
  }

  if (fdwReason == crt::kDllProcessDetach) {
    // lpvReserved: nullptr = FreeLibrary, non-nullptr = process terminating
    //
    // For __cxa_finalize: libc++abi maintains a global destructor list.
    // - On explicit unload (FreeLibrary): call with __dso_handle to run
    //   only this DLL's static destructors.
    // - On process termination: exe's exit() calls __cxa_finalize(nullptr)
    //   which handles all remaining destructors.
    //
    // Note: We only run cleanup on explicit unload. During process
    // termination, the exe's exit() handles cleanup for all loaded modules.
    if (lpvReserved == nullptr) {
      // Run C++ static destructors for this DLL only
      CRT_CXA_FINALIZE_CALL(__dso_handle);

      // Run pre-terminators and terminators for this DLL
      // These are module-local due to the section mechanism
      crt::runPreterminators();
      crt::runTerminators();
    }
  }

  return DllMain(hinstDLL, fdwReason, lpvReserved);
}
#endif // CRT_HAS_DLLMAIN

#endif // _WIN32
