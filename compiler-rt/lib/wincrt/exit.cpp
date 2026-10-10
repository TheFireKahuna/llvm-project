//===-- exit.cpp - Process termination ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// exit, _exit and _Exit, which every image's entry object wraps with these
// definitions. They end the process as the Universal CRT does: its own C
// termination, run by _cexit or _c_exit, then the app model's termination
// policy. The Universal CRT reads that policy through an API set whose host,
// kernel.appcore.dll, it loads at exit together with msvcrt.dll. kernel32.dll
// forwards the same function to kernelbase.dll, which kernel.appcore.dll
// itself calls and every Win32 process has loaded, so importing it from there
// loads nothing.
//
// quick_exit stays the Universal CRT's: its at_quick_exit table has no public
// form that runs it and returns.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <appmodel.h>
#include <process.h>

// Exported by ntdll; no SDK header declares it.
extern "C" NTSYSAPI ULONG NTAPI RtlGetNtGlobalFlags(void);

namespace {

// Whether the app model asks the process to end with TerminateProcess, as it
// asks a packaged app that is not full trust, so that no other thread runs
// while the process is torn down. The Universal CRT asks nothing in a secure
// process, and ends every process with ExitProcess under Application
// Verifier, whose checks need the DLLs detached.
bool shouldTerminateProcess() {
  if (wincrt::isSecureProcess())
    return false;
  AppPolicyProcessTerminationMethod Policy =
      AppPolicyProcessTerminationMethod_ExitProcess;
  AppPolicyGetProcessTerminationMethod(GetCurrentThreadEffectiveToken(),
                                       &Policy);
  // FLG_APPLICATION_VERIFIER, which no SDK header defines.
  constexpr ULONG ApplicationVerifier = 0x100;
  return Policy == AppPolicyProcessTerminationMethod_TerminateProcess &&
         !(RtlGetNtGlobalFlags() & ApplicationVerifier);
}

// The common language runtime ends a process it hosts itself. The Universal
// CRT asks it only when mscoree.dll is already loaded, and loads nothing. The
// call goes to a function of the system, which has no KCFI type.
__attribute__((no_sanitize("kcfi"))) void tryCorExitProcess(int Code) {
  HMODULE Mscoree;
  if (!GetModuleHandleExW(0, L"mscoree.dll", &Mscoree))
    return;
  if (auto CorExitProcess = reinterpret_cast<void(WINAPI *)(int)>(
          GetProcAddress(Mscoree, "CorExitProcess")))
    CorExitProcess(Code);
  FreeLibrary(Mscoree);
}

[[noreturn]] void endProcess(int Code) {
  if (shouldTerminateProcess())
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(Code));
  tryCorExitProcess(Code);
  ExitProcess(static_cast<UINT>(Code));
}

} // namespace

extern "C" {

WINCRT_ATEXIT_API void __cdecl __wrap_exit(int Code) {
  _cexit();
  endProcess(Code);
}

WINCRT_ATEXIT_API void __cdecl __wrap__exit(int Code) {
  _c_exit();
  endProcess(Code);
}

WINCRT_ATEXIT_API void __cdecl __wrap__Exit(int Code) {
  _c_exit();
  endProcess(Code);
}

} // extern "C"
