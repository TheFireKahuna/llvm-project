// Uncaught exceptions must reach libc++abi's installed terminate handler.
// RUN: %clangxx_crt_main -std=c++17 -O2 %s -o %t.exe
// RUN: %run %t.exe main 2>&1 | FileCheck %s
// RUN: %run %t.exe thread 2>&1 | FileCheck %s
// RUN: %clangxx_crt_wmain -std=c++17 -O2 -DENTRY_WMAIN %s -o %t.wide.exe
// RUN: %run %t.wide.exe main 2>&1 | FileCheck %s
// RUN: %clangxx_crt_winmain -std=c++17 -O2 -DENTRY_WINMAIN %s -o %t.gui.exe
// RUN: %run %t.gui.exe 2>&1 | FileCheck %s
// RUN: %clangxx_crt_wwinmain -std=c++17 -O2 -DENTRY_WWINMAIN %s -o %t.wide-gui.exe
// RUN: %run %t.wide-gui.exe 2>&1 | FileCheck %s
// REQUIRES: windows, crt

#include <exception>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <windows.h>

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
  fprintf(stderr, "installed terminate handler; active exception = 42\n");
  _Exit(0);
}

static LONG WINAPI unexpectedFilter(EXCEPTION_POINTERS *) { _Exit(5); }

#if defined(ENTRY_WMAIN)
int wmain(int argc, wchar_t **argv) {
#elif defined(ENTRY_WINMAIN)
extern "C" int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#elif defined(ENTRY_WWINMAIN)
extern "C" int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
#else
int main(int argc, char **argv) {
#endif
  SetUnhandledExceptionFilter(unexpectedFilter);
  std::set_terminate(terminateHandler);
#if !defined(ENTRY_WINMAIN) && !defined(ENTRY_WWINMAIN)
  if (argc != 2)
    return 1;
  if (argv[1][0] == 't') {
    std::thread([] { throw 42; }).join();
    return 1;
  }
#endif
  throw 42;
}

// CHECK: installed terminate handler; active exception = 42
