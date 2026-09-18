// Public quick-exit registration: ordering, reentry, unload and exceptions.
// RUN: %clangxx_crt_dll -O2 -DBUILD_DLL %s -o %t.dll
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe order %t.dll 2>&1 | FileCheck %s --check-prefix=ORDER
// RUN: %run %t.exe unload %t.dll 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t.exe normal %t.dll 2>&1 | FileCheck %s --check-prefix=NORMAL
// RUN: %run %t.exe reenter %t.dll 2>&1 | FileCheck %s --check-prefix=REENTER
// RUN: %run %t.exe parallel %t.dll 2>&1 | FileCheck %s --check-prefix=COUNT
// RUN: %run %t.exe throwing %t.dll 2>&1 | FileCheck %s --check-prefix=THROW
// RUN: %clang_crt_main -x c -O2 -UNDEBUG -DBUILD_C_HOST %s -o %t.c.exe
// RUN: %run %t.c.exe %t.dll 2>&1 | FileCheck %s --check-prefix=C-HOST
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#if defined(BUILD_DLL)
static void quick() { fprintf(stderr, "quick DLL\n"); }
static void normal() { fprintf(stderr, "normal DLL\n"); }
extern "C" __declspec(dllexport) int install() {
  return atexit(normal) || at_quick_exit(quick);
}
#elif defined(BUILD_C_HOST)
int main(int argc, char **argv) {
  assert(argc == 2);
  HMODULE module = LoadLibraryA(argv[1]);
  assert(module);
  int (*install)(void) = (int (*)(void))GetProcAddress(module, "install");
  assert(install && install() == 0);
  assert(FreeLibrary(module));
  assert(!GetModuleHandleA(argv[1]));
  fprintf(stderr, "unloaded from C host\n");
  quick_exit(0);
}
// C-HOST: normal DLL
// C-HOST-NEXT: unloaded from C host
// C-HOST-NOT: quick
#else
#  include <exception>
#  include <thread>

static void first() { fprintf(stderr, "quick first\n"); }
static void last() { fprintf(stderr, "quick last\n"); }
static void added() { fprintf(stderr, "quick added\n"); }
static void reenter() {
  fprintf(stderr, "quick reenter\n");
  assert(at_quick_exit(added) == 0);
}
static int calls;
static void count() { ++calls; }
static void report() {
  assert(calls == 128);
  fprintf(stderr, "quick count = %d\n", calls);
}
static void throwing() { throw 42; }
static void terminateHandler() {
  auto exception = std::current_exception();
  assert(exception);
  try {
    std::rethrow_exception(exception);
  } catch (int value) {
    assert(value == 42);
  } catch (...) {
    _Exit(3);
  }
  fprintf(stderr, "terminate; active exception = 42\n");
  _Exit(0);
}
int main(int argc, char **argv) {
  assert(argc == 3);
  const char mode = argv[1][0];
  if (mode == 'p') {
    assert(at_quick_exit(report) == 0);
    std::thread threads[4];
    for (auto &thread : threads)
      thread = std::thread([] {
        for (int i = 0; i < 32; ++i)
          assert(at_quick_exit(count) == 0);
      });
    for (auto &thread : threads)
      thread.join();
  } else if (mode == 't') {
    std::set_terminate(terminateHandler);
    assert(at_quick_exit(throwing) == 0);
  } else {
    assert(at_quick_exit(first) == 0);
    if (mode == 'r') {
      assert(at_quick_exit(reenter) == 0);
    } else {
      HMODULE module = LoadLibraryA(argv[2]);
      assert(module);
      auto install =
          reinterpret_cast<int (*)()>(GetProcAddress(module, "install"));
      assert(install && install() == 0);
      if (mode == 'u') {
        assert(FreeLibrary(module));
        assert(!GetModuleHandleA(argv[2]));
      }
      assert(at_quick_exit(last) == 0);
    }
  }
  fprintf(stderr, "registrations complete\n");
  if (mode == 'n')
    return 0;
  try {
    quick_exit(0);
  } catch (...) {
    _Exit(2);
  }
}
// ORDER: registrations complete
// ORDER-NEXT: quick last
// ORDER-NEXT: quick DLL
// ORDER-NEXT: quick first
// ORDER-NOT: normal
// UNLOAD: normal DLL
// UNLOAD-NEXT: registrations complete
// UNLOAD-NEXT: quick last
// UNLOAD-NEXT: quick first
// UNLOAD-NOT: DLL
// NORMAL: registrations complete
// NORMAL-NEXT: normal DLL
// NORMAL-NOT: quick
// REENTER: registrations complete
// REENTER-NEXT: quick reenter
// REENTER-NEXT: quick added
// REENTER-NEXT: quick first
// COUNT: quick count = 128
// THROW: terminate; active exception = 42
#endif
