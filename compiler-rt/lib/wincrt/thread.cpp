//===-- thread.cpp - _beginthread and _endthread --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// _beginthread, _beginthreadex, _endthread and _endthreadex, which every
// image's entry object wraps with these definitions. They do what the
// Universal CRT's do, which read the app model's thread initialization policy
// through kernel.appcore.dll on a process's first thread.
//
// A started thread keeps a record of what its start did, which its end
// undoes: the Universal CRT keeps it in its per-thread data, which it does not
// expose, and which needs nothing from these functions. A thread that leaves
// through ExitThread rather than by returning or through _endthread keeps its
// module loaded, as it would under the Universal CRT, and leaks its record.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <errno.h>
#include <process.h>
#include <roapi.h>

using namespace wincrt;

namespace {

struct ThreadRecord {
  // _beginthreadex's procedure, or _beginthread's if Handle is set.
  _beginthreadex_proc_type Procedure;
  void *Context;
  // The image that holds the procedure, which stays loaded while it runs.
  HMODULE Module;
  // _beginthread's handle, which the thread closes as it ends.
  HANDLE Handle;
  // Whether the start initialized the Windows Runtime on the thread.
  bool Apartment;
};

thread_local ThreadRecord *Started;

// The Universal CRT's errno for the error of a failed CreateThread or
// ResumeThread.
void setErrnoFromLastError() {
  DWORD Error = GetLastError();
  _set_doserrno(Error);
  switch (Error) {
  case ERROR_ARENA_TRASHED:
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_INVALID_BLOCK:
  case ERROR_NOT_ENOUGH_QUOTA:
    errno = ENOMEM;
    break;
  case ERROR_ACCESS_DENIED:
    errno = EACCES;
    break;
  case ERROR_NO_PROC_SLOTS:
  case ERROR_MAX_THRDS_REACHED:
  case ERROR_NESTING_NOT_ALLOWED:
    errno = EAGAIN;
    break;
  default:
    errno = EINVAL;
  }
}

// Only a packaged app that is not full trust asks for the Windows Runtime on
// every thread; only there is combase.dll loaded for it. The calls go to
// functions of the system, which have no KCFI type.
__attribute__((no_sanitize("kcfi"))) bool roInitialize() {
  if (appPolicy("AppPolicyGetThreadInitializationType",
                AppPolicyThreadInitializationType_None) !=
      AppPolicyThreadInitializationType_InitializeWinRT)
    return false;
  HMODULE Combase =
      LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  auto RoInitialize = reinterpret_cast<HRESULT(WINAPI *)(RO_INIT_TYPE)>(
      Combase ? GetProcAddress(Combase, "RoInitialize") : nullptr);
  return RoInitialize && RoInitialize(RO_INIT_MULTITHREADED) == S_OK;
}

__attribute__((no_sanitize("kcfi"))) void roUninitialize() {
  if (auto RoUninitialize = reinterpret_cast<void(WINAPI *)()>(
          GetProcAddress(GetModuleHandleW(L"combase.dll"), "RoUninitialize")))
    RoUninitialize();
}

[[noreturn]] void endThread(unsigned Code) {
  ThreadRecord *T = Started;
  if (!T)
    ExitThread(Code);
  Started = nullptr;
  if (T->Apartment)
    roUninitialize();
  if (T->Handle)
    CloseHandle(T->Handle);
  HMODULE Module = T->Module;
  crtFree(T);
  if (Module)
    FreeLibraryAndExitThread(Module, Code);
  ExitThread(Code);
}

// A hardware exception that no frame handles goes to the Universal CRT's
// signal handlers, and one they choose to handle ends the program with its
// code, as on a thread the Universal CRT starts. The procedure may come from
// an object built without KCFI; Control Flow Guard checks the call.
__attribute__((no_sanitize("kcfi"))) DWORD WINAPI threadStart(void *Record) {
  auto *T = Started = static_cast<ThreadRecord *>(Record);
  T->Apartment = roInitialize();
  __try {
    if (!T->Handle)
      endThread(T->Procedure(T->Context));
    reinterpret_cast<_beginthread_proc_type>(T->Procedure)(T->Context);
    endThread(0);
  } __except (_seh_filter_exe(GetExceptionCode(), GetExceptionInformation())) {
    _exit(static_cast<int>(GetExceptionCode()));
  }
}

// Starts a thread for _beginthreadex, or with Close set for _beginthread,
// whose thread closes its own handle and so learns it before it runs. Returns
// null on failure, with errno set.
HANDLE beginThread(_beginthreadex_proc_type Procedure, bool Close,
                   void *Security, unsigned StackSize, void *Context,
                   unsigned Flags, unsigned *ThreadId) {
  if (!Procedure) {
    errno = EINVAL;
    _invalid_parameter_noinfo();
    return nullptr;
  }
  auto *T = static_cast<ThreadRecord *>(crtAlloc(sizeof(ThreadRecord)));
  if (!T) {
    errno = ENOMEM;
    return nullptr;
  }
  T->Procedure = Procedure;
  T->Context = Context;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                     reinterpret_cast<LPCWSTR>(Procedure), &T->Module);
  DWORD Id;
  HANDLE Thread = CreateThread(static_cast<LPSECURITY_ATTRIBUTES>(Security),
                               StackSize, threadStart, T, Flags, &Id);
  if (!Thread) {
    setErrnoFromLastError();
    if (T->Module)
      FreeLibrary(T->Module);
    crtFree(T);
    return nullptr;
  }
  if (ThreadId)
    *ThreadId = Id;
  if (Close) {
    T->Handle = Thread;
    if (ResumeThread(Thread) == static_cast<DWORD>(-1)) {
      setErrnoFromLastError();
      return nullptr;
    }
  }
  return Thread;
}

} // namespace

extern "C" {

WINCRT_API uintptr_t __cdecl
__wrap__beginthread(_beginthread_proc_type Procedure, unsigned StackSize,
                    void *Context) {
  HANDLE Thread =
      beginThread(reinterpret_cast<_beginthreadex_proc_type>(Procedure), true,
                  nullptr, StackSize, Context, CREATE_SUSPENDED, nullptr);
  return reinterpret_cast<uintptr_t>(Thread ? Thread : INVALID_HANDLE_VALUE);
}

WINCRT_API uintptr_t __cdecl
__wrap__beginthreadex(void *Security, unsigned StackSize,
                      _beginthreadex_proc_type Procedure, void *Context,
                      unsigned Flags, unsigned *ThreadId) {
  return reinterpret_cast<uintptr_t>(beginThread(
      Procedure, false, Security, StackSize, Context, Flags, ThreadId));
}

WINCRT_API void __cdecl __wrap__endthreadex(unsigned Code) { endThread(Code); }

WINCRT_API void __cdecl __wrap__endthread(void) { endThread(0); }

} // extern "C"
