// An exception no handler catches reaches std::terminate before any frame is
// unwound, from every kind of thread, and never reaches the program's Win32
// unhandled-exception filter. Every other unhandled exception still does, and
// so does a foreign raise of the unwinder's own exception code.
// RUN: %clangxx_crt_main -std=c++17 -O2 %s -o %t.exe
// RUN: %run %t.exe main 2>&1 | FileCheck %s --check-prefix=TERMINATE
// RUN: %run %t.exe thread 2>&1 | FileCheck %s --check-prefix=TERMINATE
// RUN: %run %t.exe native 2>&1 | FileCheck %s --check-prefix=TERMINATE
// RUN: %run %t.exe pool 2>&1 | FileCheck %s --check-prefix=TERMINATE
// RUN: %run %t.exe rethrow 2>&1 | FileCheck %s --check-prefix=TERMINATE
// RUN: %run %t.exe foreign 2>&1 | FileCheck %s --check-prefix=FOREIGN
// RUN: %run %t.exe fault 2>&1 | FileCheck %s --check-prefix=FAULT
// REQUIRES: windows, crt

#include <exception>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <windows.h>

static bool destroyed;

struct Guard {
  ~Guard() { destroyed = true; }
};

static void terminateHandler() {
  auto exception = std::current_exception();
  if (!exception)
    _Exit(2);
  try {
    std::rethrow_exception(exception);
  } catch (int value) {
    if (value != 42)
      _Exit(3);
  } catch (...) {
    _Exit(4);
  }
  fprintf(stderr, "terminate: active exception 42, frames intact %d\n",
          !destroyed);
  _Exit(0);
}

static LONG WINAPI filter(EXCEPTION_POINTERS *Pointers) {
  fprintf(stderr, "filter: code %#lx\n",
          Pointers->ExceptionRecord->ExceptionCode);
  _Exit(0);
}

static void throwing() {
  Guard guard;
  throw 42;
}

static void rethrowing() {
  Guard guard;
  try {
    throw 42;
  } catch (...) {
    throw;
  }
}

static DWORD WINAPI nativeThread(void *) {
  throwing();
  return 0;
}

static void CALLBACK poolCallback(PTP_CALLBACK_INSTANCE, void *) {
  throwing();
}

static DWORD WINAPI foreignThread(void *) {
  ULONG_PTR argument = 1;
  RaiseException(0x20474343, 0, 1, &argument);
  return 0;
}

static DWORD WINAPI faultThread(void *) {
  *static_cast<volatile int *>(nullptr) = 0;
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  SetUnhandledExceptionFilter(filter);
  std::set_terminate(terminateHandler);
  const char *mode = argv[1];
  if (!strcmp(mode, "main"))
    throwing();
  if (!strcmp(mode, "rethrow"))
    rethrowing();
  if (!strcmp(mode, "thread"))
    std::thread(throwing).join();
  if (!strcmp(mode, "pool")) {
    if (!TrySubmitThreadpoolCallback(poolCallback, nullptr, nullptr))
      return 1;
    Sleep(INFINITE);
  }
  LPTHREAD_START_ROUTINE routine = !strcmp(mode, "native")    ? nativeThread
                                   : !strcmp(mode, "foreign") ? foreignThread
                                   : !strcmp(mode, "fault")   ? faultThread
                                                              : nullptr;
  if (!routine)
    return 1;
  HANDLE thread = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr);
  if (!thread)
    return 1;
  WaitForSingleObject(thread, INFINITE);
  fprintf(stderr, "thread returned\n");
  return 1;
}

// TERMINATE: terminate: active exception 42, frames intact 1
// FOREIGN: filter: code 0x20474343
// FAULT: filter: code 0xc0000005
