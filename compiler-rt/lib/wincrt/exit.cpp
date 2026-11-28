//===-- exit.cpp - exit, quick_exit, raise and abort ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The functions that end the process, which every image's entry object wraps
// with these definitions. The Universal CRT's own end it through a policy
// query that loads kernel.appcore.dll and msvcrt.dll. These run its C
// termination, through _cexit or _c_exit, and then end the process as it
// would.
//
// raise and abort are here because the Universal CRT's end the process, for a
// signal's default action or with _CALL_REPORTFAULT cleared, through its own
// _exit, which the wrap of _exit cannot reach.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <process.h>
#include <signal.h>

extern "C" {
// Exported by ntdll; no SDK header declares it.
NTSYSAPI ULONG NTAPI RtlGetNtGlobalFlags(void);

// The Universal CRT's functions, which the wraps bind to these names.
[[noreturn]] void __cdecl __real_quick_exit(int);
int __cdecl __real_raise(int);
}

// The common language runtime ends a process it hosts itself. The Universal
// CRT asks it only when mscoree.dll is already loaded, and loads nothing. The
// call goes to a function of the system, which has no KCFI type.
__attribute__((no_sanitize("kcfi"))) static void tryCorExitProcess(int Code) {
  HMODULE Mscoree;
  if (!GetModuleHandleExW(0, L"mscoree.dll", &Mscoree))
    return;
  if (auto CorExitProcess = reinterpret_cast<void(WINAPI *)(int)>(
          GetProcAddress(Mscoree, "CorExitProcess")))
    CorExitProcess(Code);
  FreeLibrary(Mscoree);
}

// The app model asks a packaged app that is not full trust to end with
// TerminateProcess, so that no other thread runs while the process is torn
// down. Under Application Verifier, whose checks need the DLLs detached, the
// Universal CRT ends every process with ExitProcess.
[[noreturn]] static void endProcess(int Code) {
  // FLG_APPLICATION_VERIFIER, which no SDK header defines.
  constexpr ULONG ApplicationVerifier = 0x100;
  if (wincrt::appPolicy("AppPolicyGetProcessTerminationMethod",
                        AppPolicyProcessTerminationMethod_ExitProcess) ==
          AppPolicyProcessTerminationMethod_TerminateProcess &&
      !(RtlGetNtGlobalFlags() & ApplicationVerifier))
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(Code));
  tryCorExitProcess(Code);
  ExitProcess(static_cast<UINT>(Code));
}

extern "C" {

WINCRT_API void __cdecl __wrap_exit(int Code) {
  // The first thread to exit ends the process; another's exit never returns.
  // A function that the first's drain calls continues the drain.
  LONG Self = static_cast<LONG>(GetCurrentThreadId()), Owner = 0;
  if (!__atomic_compare_exchange_n(&wincrt::exitState().Drained, &Owner, Self, false,
                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) &&
      (Owner & ~3) != Self)
    for (;;)
      Sleep(INFINITE);
  if (Owner == (Self | 2))
    __cxa_finalize(nullptr);
  _cexit();
  endProcess(Code);
}

WINCRT_API void __cdecl __wrap__exit(int Code) {
  _c_exit();
  endProcess(Code);
}

WINCRT_API void __cdecl __wrap__Exit(int)
    __attribute__((alias("__wrap__exit")));

// Under an executable that reserves the process's exit list, the Universal
// CRT's table holds no DLL's registration, and the list holds every image's.
WINCRT_API void __cdecl __wrap_quick_exit(int Code) {
  if (!wincrt::runQuickExit())
    __real_quick_exit(Code);
  endProcess(Code);
}

WINCRT_API int __cdecl __wrap_raise(int Signal) {
  switch (Signal) {
  case SIGINT:
  case SIGILL:
  case SIGABRT_COMPAT:
  case SIGFPE:
  case SIGSEGV:
  case SIGTERM:
  case SIGBREAK:
  case SIGABRT:
    // SIG_GET reads the action as the Universal CRT's raise does, under its
    // lock for the process-wide signals. Every other case is that raise's:
    // SIG_IGN, the handler call and the reset before it, and the error for a
    // signal it does not support.
    if (signal(Signal, SIG_GET) == SIG_DFL)
      _exit(3);
  }
  return __real_raise(Signal);
}

// abort as the Universal CRT's release build does it: a SIGABRT handler runs
// first, through raise; then the process fails fast if _CALL_REPORTFAULT is
// set, and otherwise ends with code 3.
[[noreturn]] WINCRT_API void __cdecl __wrap_abort(void) {
  if (signal(SIGABRT, SIG_GET) != SIG_DFL)
    raise(SIGABRT);
  // The flags are read after the handler, which may change them. An empty
  // mask reads them and stores them back unchanged.
  if (_set_abort_behavior(0, 0) & _CALL_REPORTFAULT)
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  _exit(3);
}

} // extern "C"
