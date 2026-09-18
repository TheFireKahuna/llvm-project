// Optimized EXE startup must install cleanup before running constructors.
// Exercise each entry point, both returning and calling exit explicitly.
// RUN: %clangxx_crt_main -O2 %s -o %t.main.exe
// RUN: %run %t.main.exe | FileCheck %s -DENTRY=main
// RUN: env WINCRT_TEST_EXPLICIT_EXIT=1 %run %t.main.exe | FileCheck %s -DENTRY=main
// RUN: env WINCRT_TEST_EXPLICIT_EXIT=2 %run %t.main.exe | FileCheck %s --check-prefix=IMMEDIATE
// RUN: %clangxx_crt_wmain -O2 %s -o %t.wmain.exe
// RUN: %run %t.wmain.exe | FileCheck %s -DENTRY=wmain
// RUN: env WINCRT_TEST_EXPLICIT_EXIT=1 %run %t.wmain.exe | FileCheck %s -DENTRY=wmain
// RUN: %clangxx_crt_winmain -O2 %s -o %t.winmain.exe
// RUN: %run %t.winmain.exe | FileCheck %s -DENTRY=WinMain
// RUN: env WINCRT_TEST_EXPLICIT_EXIT=1 %run %t.winmain.exe | FileCheck %s -DENTRY=WinMain
// RUN: %clangxx_crt_wwinmain -O2 %s -o %t.wwinmain.exe
// RUN: %run %t.wwinmain.exe | FileCheck %s -DENTRY=wWinMain
// RUN: env WINCRT_TEST_EXPLICIT_EXIT=1 %run %t.wwinmain.exe | FileCheck %s -DENTRY=wWinMain
// RUN: %clangxx_crt_main -O2 -DEXIT_IN_CONSTRUCTOR %s -o %t.ctor.exe
// RUN: %run %t.ctor.exe | FileCheck %s -DENTRY=constructor
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

struct Object {
  int id;
  Object(int value) : id(value) { printf("construct %d\n", id); }
  ~Object() { printf("destroy %d\n", id); }
};

Object first(1), second(2);

#ifdef EXIT_IN_CONSTRUCTOR
struct EarlyExit {
  EarlyExit() {
    puts("constructor exit");
    exit(0);
  }
} earlyExit;
#endif

int run(const char *entry) {
  printf("%s exit\n", entry);
  if (const char *mode = getenv("WINCRT_TEST_EXPLICIT_EXIT")) {
    if (*mode == '2') {
      fflush(stdout);
      _Exit(0);
    }
    exit(0);
  }
  return 0;
}

int main() { return run("main"); }
int wmain() { return run("wmain"); }
extern "C" int __stdcall WinMain(void *, void *, char *, int) {
  return run("WinMain");
}
extern "C" int __stdcall wWinMain(void *, void *, wchar_t *, int) {
  return run("wWinMain");
}

// CHECK: construct 1
// CHECK-NEXT: construct 2
// CHECK-NEXT: [[ENTRY]] exit
// CHECK-NEXT: destroy 2
// CHECK-NEXT: destroy 1
// CHECK-NOT: destroy
// IMMEDIATE: main exit
// IMMEDIATE-NOT: destroy
