// _Unwind_ForcedUnwind runs every cleanup between its caller and the end of
// the thread's stack, including a __finally block and whatever the C runtime
// and the system put under the thread's first frame, and then reports the end
// of the stack to its stop function.
// RUN: %clangxx_crt_main -std=c++17 -O2 %s -o %t.exe
// RUN: %run %t.exe main 2>&1 | FileCheck %s
// RUN: %run %t.exe thread 2>&1 | FileCheck %s
// RUN: %run %t.exe native 2>&1 | FileCheck %s
// RUN: %run %t.exe beginthreadex 2>&1 | FileCheck %s
// REQUIRES: windows, crt

#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unwind.h>
#include <windows.h>

static int cleanups;

struct Guard {
  ~Guard() {
    ++cleanups;
    fprintf(stderr, "destructor\n");
  }
};

static _Unwind_Reason_Code stop(int, _Unwind_Action actions, uint64_t,
                                _Unwind_Exception *, _Unwind_Context *,
                                void *) {
  if (actions & _UA_END_OF_STACK) {
    fprintf(stderr, "end of stack after %d cleanups\n", cleanups);
    _Exit(0);
  }
  return _URC_NO_REASON;
}

static void forcedUnwind() {
  auto *exception = new _Unwind_Exception;
  memset(exception, 0, sizeof(*exception));
  _Unwind_ForcedUnwind(exception, stop, nullptr);
  abort();
}

__attribute__((noinline)) static void inner() {
  Guard guard;
  forcedUnwind();
}

// A frame with a termination handler and no C++ objects.
static void finallyFrame() {
  __try {
    inner();
  } __finally {
    ++cleanups;
    fprintf(stderr, "finally\n");
  }
}

static DWORD WINAPI nativeThread(void *) {
  finallyFrame();
  return 0;
}

static unsigned __stdcall beginthreadexThread(void *) {
  finallyFrame();
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  const char *mode = argv[1];
  if (!strcmp(mode, "main"))
    finallyFrame();
  if (!strcmp(mode, "thread"))
    std::thread(finallyFrame).join();
  HANDLE thread = nullptr;
  if (!strcmp(mode, "native"))
    thread = CreateThread(nullptr, 0, nativeThread, nullptr, 0, nullptr);
  if (!strcmp(mode, "beginthreadex"))
    thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, beginthreadexThread, nullptr, 0, nullptr));
  if (!thread)
    return 1;
  WaitForSingleObject(thread, INFINITE);
  fprintf(stderr, "thread returned\n");
  return 1;
}

// CHECK: destructor
// CHECK-NEXT: finally
// CHECK-NEXT: end of stack after 2 cleanups
