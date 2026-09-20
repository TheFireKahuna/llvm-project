//===-- init.cpp - Image initialization and process termination -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Termination follows [basic.start.term] and Itanium C++ ABI 3.3.6. Normal
// exit destroys the exiting thread's thread-local objects, then runs static
// destructors and atexit callbacks in reverse registration order across every
// image, then the pre-terminators and terminators. UCRT drives this: the
// callback that executable startup registers runs first and drains the
// thread-locals, UCRT's atexit table then runs the registry's token, which
// drains the statics and calls back here for the terminators. DLL images
// finalize their own registrations at detach (entry_dll.cpp).
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <process.h>

namespace wincrt {

namespace {

INIT_ONCE InitializeOnce = INIT_ONCE_STATIC_INIT;
LONG ExitCleanupRan;

BOOL __stdcall initializeOnce(PINIT_ONCE, void *, void **) {
  applyImportFixups();
  bindWeakDefinitions();
  if (_initterm_e(__xi_a, __xi_z) != 0)
    return FALSE;
  _initterm(__xc_a, __xc_z);
  return TRUE;
}

// Runs before UCRT's atexit table on the exiting thread.
void __stdcall exitCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_DETACH)
    __cxa_thread_finalize(nullptr);
}

// Runs from the registry's token after every static registration.
void runTerminators() {
  if (InterlockedExchange(&ExitCleanupRan, 1))
    return;
  _initterm(__xp_a, __xp_z);
  _initterm(__xt_a, __xt_z);
}

int entryFilter(EXCEPTION_POINTERS *Exception) {
  terminateFilter(Exception);
  // UCRT converts hardware exceptions into signal() dispatch for the program.
  return _seh_filter_exe(Exception->ExceptionRecord->ExceptionCode, Exception);
}

void writeStderr(const char *Text, size_t Length) {
  HANDLE Handle = GetStdHandle(STD_ERROR_HANDLE);
  DWORD Written;
  if (Handle && Handle != INVALID_HANDLE_VALUE)
    WriteFile(Handle, Text, static_cast<DWORD>(Length), &Written, nullptr);
}

} // namespace

bool initializeImage() {
  return InitOnceExecuteOnce(&InitializeOnce, initializeOnce, nullptr,
                             nullptr) != FALSE;
}

void executableInit() {
  // UCRT accepts one callback per process. Register it before constructors,
  // which may call exit themselves.
  _register_thread_local_exe_atexit_callback(exitCallback);
  __wincrt_register_executable(runTerminators);
  if (!initializeImage())
    fatalError(RuntimeError::CrtNotInit);
}

void detachImage(bool Terminating) {
  if (!__wincrt_detach_image(__dso_handle, Terminating))
    return;
  _initterm(__xp_a, __xp_z);
  _initterm(__xt_a, __xt_z);
}

void runExecutable(_crt_app_type Type,
                   int(__cdecl *ConfigureArgv)(_crt_argv_mode),
                   int(__cdecl *InitializeEnvironment)(void),
                   int (*Invoke)(void)) {
  __try {
    _set_app_type(Type);
    if (ConfigureArgv(_crt_argv_unexpanded_arguments) != 0)
      fatalError(RuntimeError::SpaceArg);
    if (InitializeEnvironment() != 0)
      fatalError(RuntimeError::SpaceEnv);
    executableInit();
    exit(Invoke());
  } __except (entryFilter(GetExceptionInformation())) {
    // A SIG_DFL hardware exception with no handler ends the program with the
    // exception code, as vcruntime does.
    _exit(static_cast<int>(GetExceptionCode()));
  }
}

void fatalError(RuntimeError Error) { _amsg_exit(static_cast<int>(Error)); }

} // namespace wincrt

extern "C" {

// vcruntime reports fatal startup errors as "R6nnn" and exits with 255,
// bypassing every atexit handler and destructor.
void __cdecl _amsg_exit(int Code) {
  char Message[] = "runtime error R6000\n";
  char *Digits = Message + sizeof("runtime error R6") - 1;
  for (int Value = Code < 0 ? -Code : Code, I = 2; I >= 0 && Value; --I) {
    Digits[I] = static_cast<char>('0' + Value % 10);
    Value /= 10;
  }
  OutputDebugStringA(Message);
  wincrt::writeStderr(Message, sizeof(Message) - 1);
  _exit(255);
}

// vcruntime compatibility for C code that links wincrt.
int __cdecl _is_c_termination_complete(void) {
  return InterlockedCompareExchange(&wincrt::ExitCleanupRan, 0, 0) != 0;
}

// vcruntime-style initialization for DLLs with a custom entry point.
BOOL __stdcall _CRT_INIT(HINSTANCE, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH) {
    __security_init_cookie();
    return wincrt::initializeImage();
  }
  if (Reason == DLL_PROCESS_DETACH)
    wincrt::detachImage(Reserved != nullptr);
  return TRUE;
}

} // extern "C"
