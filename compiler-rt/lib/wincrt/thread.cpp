//===-- thread.cpp - Thread start and end ---------------------------------===//
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
// through an API set whose host, kernel.appcore.dll, they load on a process's
// first thread together with msvcrt.dll. kernel32.dll forwards the same
// function to kernelbase.dll, which every Win32 process has loaded.
//
// A started thread keeps a record of what its start did, which its end undoes,
// in a TLS slot: the Universal CRT keeps it in its per-thread data, which it
// does not expose. That per-thread data needs nothing from these functions:
// the Universal CRT makes it when the thread first uses it and frees it at
// the thread's detach. A thread that leaves through ExitThread rather than by
// returning or through _endthread keeps its module loaded, as it would under
// the Universal CRT, and leaks its record.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <appmodel.h>
#include <errno.h>
#include <process.h>
#include <roapi.h>

namespace {

struct ThreadRecord {
  void *Procedure;
  void *Context;
  // The image that holds the procedure, which stays loaded while it runs.
  HMODULE Module;
  // _beginthread's handle, which the thread closes as it ends.
  HANDLE Handle;
  // Whether the start initialized the Windows Runtime on the thread.
  bool Apartment;
};

DWORD Slot = TLS_OUT_OF_INDEXES;

DWORD recordSlot() {
  DWORD Current = __atomic_load_n(&Slot, __ATOMIC_ACQUIRE);
  if (Current != TLS_OUT_OF_INDEXES)
    return Current;
  DWORD Fresh = TlsAlloc();
  if (Fresh == TLS_OUT_OF_INDEXES)
    return Fresh;
  if (__atomic_compare_exchange_n(&Slot, &Current, Fresh, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return Fresh;
  TlsFree(Fresh);
  return Current;
}

// The Universal CRT's errno for the error of a failed CreateThread or
// ResumeThread.
void setErrnoFromOsError(DWORD Error) {
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
    break;
  }
}

ThreadRecord *makeRecord(void *Procedure, void *Context) {
  auto *T = static_cast<ThreadRecord *>(wincrt::crtAlloc(sizeof(ThreadRecord)));
  if (!T) {
    errno = ENOMEM;
    return nullptr;
  }
  *T = ThreadRecord();
  T->Procedure = Procedure;
  T->Context = Context;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                     static_cast<LPCWSTR>(Procedure), &T->Module);
  return T;
}

void freeRecord(ThreadRecord *T) {
  if (T->Handle)
    CloseHandle(T->Handle);
  if (T->Module)
    FreeLibrary(T->Module);
  wincrt::crtFree(T);
}

// Only a packaged app that is not full trust asks for the Windows Runtime on
// every thread; only there is combase.dll loaded for it. The answer is the
// process's for its lifetime, so it is asked once. The calls go to functions
// of the system, which have no KCFI type.
__attribute__((no_sanitize("kcfi"))) bool roInitialize() {
  enum : LONG { Unknown, NoRuntime, Runtime };
  static LONG Policy = Unknown;
  LONG Current = __atomic_load_n(&Policy, __ATOMIC_ACQUIRE);
  if (Current == Unknown) {
    AppPolicyThreadInitializationType Type =
        AppPolicyThreadInitializationType_None;
    if (!wincrt::isSecureProcess())
      AppPolicyGetThreadInitializationType(GetCurrentThreadEffectiveToken(),
                                           &Type);
    Current = Type == AppPolicyThreadInitializationType_InitializeWinRT
                  ? Runtime
                  : NoRuntime;
    __atomic_store_n(&Policy, Current, __ATOMIC_RELEASE);
  }
  if (Current != Runtime)
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
  DWORD S = __atomic_load_n(&Slot, __ATOMIC_ACQUIRE);
  auto *T = S == TLS_OUT_OF_INDEXES
                ? nullptr
                : static_cast<ThreadRecord *>(TlsGetValue(S));
  if (!T)
    ExitThread(Code);
  TlsSetValue(S, nullptr);
  if (T->Apartment)
    roUninitialize();
  HMODULE Module = T->Module;
  T->Module = nullptr;
  freeRecord(T);
  if (Module)
    FreeLibraryAndExitThread(Module, Code);
  ExitThread(Code);
}

// A hardware exception that no frame handles goes to the Universal CRT's
// signal handlers, and one they choose to handle ends the program with its
// code, as on a thread the Universal CRT starts. The procedure may come from
// an object built without KCFI; Control Flow Guard checks the call.
template <bool Ex>
__attribute__((no_sanitize("kcfi"))) DWORD WINAPI threadStart(void *Parameter) {
  auto *T = static_cast<ThreadRecord *>(Parameter);
  TlsSetValue(Slot, T);
  T->Apartment = roInitialize();
  __try {
    if constexpr (Ex) {
      endThread(
          reinterpret_cast<_beginthreadex_proc_type>(T->Procedure)(T->Context));
    } else {
      reinterpret_cast<_beginthread_proc_type>(T->Procedure)(T->Context);
      endThread(0);
    }
  } __except (_seh_filter_exe(GetExceptionCode(), GetExceptionInformation())) {
    _exit(static_cast<int>(GetExceptionCode()));
  }
}

} // namespace

extern "C" {

WINCRT_ATEXIT_API uintptr_t __cdecl
__wrap__beginthread(_beginthread_proc_type Procedure, unsigned StackSize,
                    void *Context) {
  if (!Procedure) {
    errno = EINVAL;
    _invalid_parameter_noinfo();
    return reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE);
  }
  if (recordSlot() == TLS_OUT_OF_INDEXES) {
    setErrnoFromOsError(GetLastError());
    return reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE);
  }
  ThreadRecord *T = makeRecord(reinterpret_cast<void *>(Procedure), Context);
  if (!T)
    return reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE);
  // The thread closes its own handle, so it learns the handle before it runs.
  HANDLE Thread = CreateThread(nullptr, StackSize, threadStart<false>, T,
                               CREATE_SUSPENDED, nullptr);
  if (!Thread) {
    setErrnoFromOsError(GetLastError());
    freeRecord(T);
    return reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE);
  }
  T->Handle = Thread;
  if (ResumeThread(Thread) == static_cast<DWORD>(-1)) {
    setErrnoFromOsError(GetLastError());
    return reinterpret_cast<uintptr_t>(INVALID_HANDLE_VALUE);
  }
  return reinterpret_cast<uintptr_t>(Thread);
}

WINCRT_ATEXIT_API uintptr_t __cdecl
__wrap__beginthreadex(void *Security, unsigned StackSize,
                      _beginthreadex_proc_type Procedure, void *Context,
                      unsigned Flags, unsigned *ThreadId) {
  if (!Procedure) {
    errno = EINVAL;
    _invalid_parameter_noinfo();
    return 0;
  }
  if (recordSlot() == TLS_OUT_OF_INDEXES) {
    setErrnoFromOsError(GetLastError());
    return 0;
  }
  ThreadRecord *T = makeRecord(reinterpret_cast<void *>(Procedure), Context);
  if (!T)
    return 0;
  DWORD Id;
  HANDLE Thread = CreateThread(static_cast<LPSECURITY_ATTRIBUTES>(Security),
                               StackSize, threadStart<true>, T, Flags, &Id);
  if (!Thread) {
    setErrnoFromOsError(GetLastError());
    freeRecord(T);
    return 0;
  }
  if (ThreadId)
    *ThreadId = Id;
  return reinterpret_cast<uintptr_t>(Thread);
}

WINCRT_ATEXIT_API void __cdecl __wrap__endthread(void) { endThread(0); }

WINCRT_ATEXIT_API void __cdecl __wrap__endthreadex(unsigned Code) {
  endThread(Code);
}

} // extern "C"
